// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac-pw — PipeWire-native REAC endpoint, MASTER or SLAVE role.
 *
 * A single libpipewire client that registers the 40-channel REAC source node
 * fed by a pcap replay (offline) or a live AF_PACKET 0x8819 socket, decoding
 * with libreac's braid core and pushing samples through a lock-free
 * ring into the realtime process() callback. The node is a follower; PipeWire's
 * adapter resamples REAC -> graph (Tier-A clock bridge).
 *
 * REAC has no fixed master: any box can be the master and the rest slave to it.
 * openmixer must fit either role, selected with --role:
 *
 *   --role master (default) — WE drive the cdea/cfea establishment + own the
 *     clock (the SCHED_FIFO pacer at a fixed pps); a stagebox slaves to us. The
 *     downstream master TX is the reac:playback sink (--tx IFNAME).
 *   --role slave — an EXTERNAL master (a desk, or a box configured as master)
 *     drives the establishment; WE RESPOND and LOCK to the master's cadence (the
 *     master owns the clock — we never run our own pacer as the timing source).
 *     We RX the master's audio (reac:capture) and TX our input channels back
 *     upstream at the box's slots (reac_slave, --tx IFNAME = the REAC NIC).
 *
 * Both roles share the encoder/decoder + the PipeWire nodes; they differ only in
 * WHO drives the handshake + the clock (master drives; slave follows).
 *
 *   reac-pw --pcap capture.pcap [--rate 48000]
 *   reac-pw --live reac0 [--rate 96000] [--role master] [--tx reac0]
 *   reac-pw --live reac0 --role slave   --tx reac0      # slaved to a desk
 *
 * ONE DAEMON, N LISTENERS (docs/design/specs/2026-08-20-reac-auto-spine.md §5,
 * the openmixer tree). A single master process manages every segment this
 * host faces, spawning one internal LISTENER per interface against ONE shared
 * PipeWire main loop — the daemon-per-NIC shape (a templated unit per
 * interface) was considered and REJECTED there. `--live` may be repeated (or
 * given as a comma list) to run several segments from one command line.
 *
 * THE PACKAGED SERVICE GIVES NO INTERFACE AT ALL, AND NONE IS CONFIGURED
 * (openmixer's 2026-08-23-reac-trunk-vlan-daemon.md §7-§9, amendment
 * 2026-09-02): with no --live/--pcap the daemon HEARS its segments. Every
 * Ethernet interface with link is sniffed by a passive 0x8819 socket; the
 * first frame that classifies as REAC gear turns that interface into a
 * segment and a listener opens on it; link loss drops it after a hold long
 * enough to ride out a box power-cycle. The segment is NAMED after its
 * interface, and nothing about it lives in a file before it is heard.
 *
 * Only the FIRST --live segment honours the per-box flags below (--tx/--role/
 * --mixer/--name/--headamp/--box/--src-mac/--box-channels/--box-model),
 * exactly as every invocation before this one; every other segment — and
 * every HEARD one — reads its own REAC_TX/REAC_ROLE/REAC_MIXER/REAC_NAME/
 * REAC_HEADAMP/REAC_BOX_CHANNELS from the layered conf, keyed by its own name
 * (reac_conf.h: `REAC_ROLE_<segment>` above `REAC_ROLE`). This is what lets a
 * single-interface invocation stay BYTE-IDENTICAL to today: a lone listener
 * is the same code path it always was, just reached through an array of one. */

#include "reac_ring.h"
#include "reac_rx.h"
#include "reac_source_node.h"
#include "reac_sink_node.h"
#include "reac_slave.h"
#include "reac_role_cfg.h"   /* the reac.cfg.role vocabulary + refusal codes */
#include "reac_role_swap.h"  /* the role swap's lifecycle answer (arbitration §8) */
#include "reac_segment_ident.h"  /* the segment identity + a slave's own answer set */
#include "reac_role.h"
#include "reac_rate_cfg.h"
#include "reac_mac.h"
#include "reac_ctrl.h"        /* enum reac_headamp_param, REAC_HEADAMP_SENS_MAX */
#include "reac_headamp_tx.h"  /* struct reac_headamp_setting */
#include "reac_box_pin.h"     /* --box MODEL[:LABEL]: the fixed-installation pin */
#include "reac_conf.h"     /* the LAYERED config lookup + which layer answered */
#include "reac_seglock.h"    /* one master per segment, across processes */
#include "reac_ifscan.h"     /* which interfaces to sniff, which are segments */
#include "reac_disco.h"      /* the sniffer's bar: a frame that IS REAC gear */

#include <pipewire/pipewire.h>
#include <reac/reac.h>
#include <reac/reac_capture.h>
#include <sys/ioctl.h>
#include <time.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <linux/if_packet.h>
#include <net/if.h>           /* IFNAMSIZ */

/* Bounded, per docs/design/specs/2026-08-20-reac-auto-spine.md ("a segment
 * beyond the bound is reported, never silently ignored") — this rig needs 2;
 * 8 is headroom for a bigger trunk without inviting an unbounded array. */
#define REAC_PW_MAX_LISTENERS 8

static struct pw_main_loop *g_loop;

static void on_signal(void *data, int sig)
{
	(void)data; (void)sig;
	pw_main_loop_quit(g_loop);
}

/* Parse "aa:bb:cc:dd:ee:ff" into out[6]; returns 0, or -1 on malformed input. */
static int parse_mac(const char *s, uint8_t out[6])
{
	unsigned b[6];
	if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x",
	           &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
		return -1;
	for (int i = 0; i < 6; i++)
		out[i] = (uint8_t)b[i];
	return 0;
}

/* Split `s` on commas and/or whitespace into up to `max` NUL-terminated
 * tokens, each truncated to IFNAMSIZ-1 bytes. Returns the token count (0 for
 * an empty/NULL/all-separator string). A token beyond `max` is REPORTED and
 * dropped, never silently lost (auto-spine's own bounding rule, applied here
 * to the list of interfaces/segments that names it). */
static int split_list(const char *s, char out[][IFNAMSIZ + 1], int max)
{
	if (!s)
		return 0;
	int n = 0;
	const char *p = s;
	while (*p) {
		while (*p == ',' || *p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		const char *start = p;
		while (*p && *p != ',' && *p != ' ' && *p != '\t')
			p++;
		size_t len = (size_t)(p - start);
		if (n >= max) {
			fprintf(stderr, "reac-pw: too many entries in '%s' (max %d); "
			        "'%.*s' is dropped\n", s, max, (int)len, start);
			continue;
		}
		if (len > (size_t)IFNAMSIZ)
			len = IFNAMSIZ;
		memcpy(out[n], start, len);
		out[n][len] = '\0';
		n++;
	}
	return n;
}

/* ---- capability preflight (trunk/VLAN spec 2026-08-23, §4e) ---------------
 *
 * CAPABILITIES ARE LOST SILENTLY, AND THIS RIG HAS ALREADY PAID FOR IT. They live
 * on the inode, so every relink drops them and the rebuilt binary looks identical
 * while being unable to open a socket. Worse, /tmp is mounted nosuid, which
 * STRIPS file capabilities with no error at all: setcap reports success, getcap
 * prints the set, and the binary still has nothing. That case cost real time and
 * is invisible unless the daemon says so, which is why it is named below.
 *
 * The check runs BEFORE anything is opened, so the failure arrives as a sentence
 * instead of as a daemon that starts, logs normally and receives nothing.
 *
 *   CAP_NET_RAW    the AF_PACKET sockets — every segment, and the detector.
 *                  Absent, nothing works: refuse.
 *   CAP_NET_ADMIN  creating, marking and removing VLAN sub-interfaces. Absent, we
 *                  can still ADOPT sub-interfaces that already exist, because
 *                  adoption needs no capability — so this is a loud refusal of
 *                  the part we cannot do, NOT an exit. A partially usable daemon
 *                  that names the missing part beats one that refuses everything.
 */
#define CAP_BIT_NET_ADMIN 12
#define CAP_BIT_NET_RAW   13

static int read_cap_effective(unsigned long long *out)
{
	FILE *f = fopen("/proc/self/status", "r");
	if (!f)
		return -1;
	char line[256];
	int got = 0;
	while (fgets(line, sizeof line, f)) {
		if (strncmp(line, "CapEff:", 7) == 0) {
			*out = strtoull(line + 7, NULL, 16);
			got = 1;
			break;
		}
	}
	fclose(f);
	return got ? 0 : -1;
}

/* 1 = the binary sits on a nosuid mount (capabilities are stripped there),
 * 0 = it does not, -1 = could not tell. */
static int path_on_nosuid(const char *path)
{
	struct statvfs vfs;
	if (!path || statvfs(path, &vfs) != 0)
		return -1;
	return (vfs.f_flag & ST_NOSUID) ? 1 : 0;
}

static void say_nosuid(const char *exe)
{
	int ns = path_on_nosuid(exe);
	if (ns == 1)
		fprintf(stderr,
		    "         AND THE BINARY IS ON A NOSUID FILESYSTEM, which strips file\n"
		    "         capabilities with no error — setcap will report success and\n"
		    "         change nothing. Move it onto a normal filesystem first\n"
		    "         (a build tree under /tmp is the usual cause).\n");
	else if (ns < 0)
		fprintf(stderr,
		    "         (could not tell whether that path is on a nosuid mount)\n");
}

/* Read our own effective set and act on what is missing. Returns with the
 * process alive only if CAP_NET_RAW is held. */
static void capability_preflight(void)
{
	char exe[4096];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (n < 0)
		n = 0;
	exe[n] = '\0';
	const char *path = n ? exe : "<path-to>/reac-pw";

	unsigned long long eff = 0;
	if (read_cap_effective(&eff) != 0) {
		fprintf(stderr, "reac-pw: could not read /proc/self/status CapEff; "
		        "skipping the capability preflight\n");
		return;
	}

	int have_raw   = (eff >> CAP_BIT_NET_RAW)   & 1ULL;
	int have_admin = (eff >> CAP_BIT_NET_ADMIN) & 1ULL;

	if (!have_raw) {
		fprintf(stderr,
		    "reac-pw: FATAL — CAP_NET_RAW is not in our effective set, so no REAC\n"
		    "         socket can be opened and this daemon would receive nothing.\n"
		    "         binary: %s\n"
		    "         fix:    sudo setcap cap_net_raw,cap_net_admin,cap_sys_nice=ep %s\n",
		    path, path);
		say_nosuid(path);
		fprintf(stderr,
		    "         Capabilities are dropped on EVERY relink; tools/build.sh sets\n"
		    "         them and verifies them. Refusing to start.\n");
		exit(1);
	}

	if (!have_admin) {
		fprintf(stderr,
		    "reac-pw: CAP_NET_ADMIN is missing. VLAN sub-interfaces cannot be\n"
		    "         created, marked or removed, so a trunk's tagged VIDs cannot be\n"
		    "         served. Sub-interfaces that ALREADY EXIST are still served —\n"
		    "         adoption needs no capability — so this is a refusal of one\n"
		    "         part, not of the daemon.\n"
		    "         binary: %s\n"
		    "         fix:    sudo setcap cap_net_raw,cap_net_admin,cap_sys_nice=ep %s\n",
		    path, path);
		say_nosuid(path);
	}
}

/* Why the AF_PACKET TX could not open, said in words the operator can act on.
 *
 * FILE CAPABILITIES LIVE ON THE INODE, AND EVERY RELINK MAKES A NEW ONE — so a
 * plain `meson compile` silently drops cap_net_raw, and a reac-pw without it
 * cannot open a raw socket. It used to carry on from there: PipeWire nodes
 * appeared, the log looked ordinary, and the box simply never synced. That is
 * the failure this refusal exists to convert into a named one. A probe socket
 * separates the two causes, because "needs CAP_NET_RAW?" with a question mark
 * made every reader check the interface first.
 *
 * Same class as the vanished-interface alarm: a daemon that cannot do its one
 * job must say so and stop, not run deaf. */
static void explain_tx_failure(const char *ifname)
{
	int s = socket(AF_PACKET, SOCK_RAW, 0);
	int no_cap = (s < 0 && errno == EPERM);
	if (s >= 0)
		close(s);

	if (no_cap) {
		fprintf(stderr,
		    "reac-pw: FATAL — no CAP_NET_RAW, so the REAC TX socket cannot open and\n"
		    "         this master would never put a frame on the wire. The binary\n"
		    "         loses its capabilities on EVERY relink; re-apply them:\n"
		    "\n"
		    "           sudo setcap cap_net_raw,cap_sys_nice=ep <path-to>/reac-pw\n"
		    "\n"
		    "         tools/build.sh does this and verifies it. Refusing to start.\n");
	} else {
		fprintf(stderr,
		    "reac-pw: FATAL — the REAC TX socket on '%s' could not open, and it is\n"
		    "         not a capability problem (a raw socket opened fine). Check the\n"
		    "         interface exists and is up: ip link show %s\n"
		    "         Refusing to start.\n", ifname, ifname);
	}
}

/* Parse "CH:PARAM:VALUE" (a master-role --headamp arg, or one cell of a
 * REAC_HEADAMP conf list) into *out. CH is the WIRE channel
 * (0..REAC_HEADAMP_MAX_CH-1); PARAM is phantom|pad|sens; VALUE is 0/1 for
 * phantom|pad and the raw SENS code 0..0x37 for sens. Returns 0, or -1 if
 * malformed / out of range. */
static int parse_headamp(const char *s, struct reac_headamp_setting *out)
{
	unsigned ch, val;
	char pstr[16];
	if (sscanf(s, "%u:%15[^:]:%u", &ch, pstr, &val) != 3)
		return -1;
	uint8_t param;
	if (!strcmp(pstr, "phantom"))
		param = REAC_HEADAMP_PHANTOM;
	else if (!strcmp(pstr, "pad"))
		param = REAC_HEADAMP_PAD;
	else if (!strcmp(pstr, "sens"))
		param = REAC_HEADAMP_SENS;
	else
		return -1;
	if (ch >= REAC_HEADAMP_MAX_CH)
		return -1;
	if (param == REAC_HEADAMP_SENS) {
		if (val > REAC_HEADAMP_SENS_MAX)
			return -1;
	} else if (val > 1) {
		return -1;
	}
	out->ch = (uint8_t)ch;
	out->param = param;
	out->value = (uint8_t)val;
	return 0;
}

/* Parse a REAC_HEADAMP conf VALUE: a whitespace/comma separated list of
 * CH:PARAM:VALUE cells (parse_headamp's own grammar) — one conf line
 * replacing the S-1608's 32 individual --headamp flags in a hand-run master.
 * Returns the cell count (0 for an empty list), or -1 on any malformed cell:
 * the WHOLE list is refused rather than applying a partial table silently,
 * the same honesty rule REAC_RATE's layer lookup already follows. */
static int parse_headamp_list(const char *s, struct reac_headamp_setting *out, int max)
{
	int n = 0;
	const char *p = s;
	while (*p) {
		while (*p == ',' || *p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		const char *start = p;
		while (*p && *p != ',' && *p != ' ' && *p != '\t')
			p++;
		size_t len = (size_t)(p - start);
		char cell[32];
		if (len >= sizeof cell || n >= max)
			return -1;
		memcpy(cell, start, len);
		cell[len] = '\0';
		if (parse_headamp(cell, &out[n]) != 0)
			return -1;
		n++;
	}
	return n;
}

static void usage(const char *p)
{
	fprintf(stderr,
	  "usage: %s [--pcap FILE | --live IFNAME] [--role master|slave] [--rate R] [--tx IFNAME]\n"
	  "         [--mixer M] [--box MODEL[:LABEL]] [--box-channels N] [--name NAME] [--src-mac M]\n"
	  "  --pcap FILE   replay a REAC capture (offline test, reuses pcap_source)\n"
	  "  --live IFNAME live AF_PACKET 0x8819 capture (reuses reac_capture; needs CAP_NET_RAW).\n"
	  "                Repeatable (or a comma list in one flag) to run several segments in\n"
	  "                this ONE daemon — see \"auto-spine\" below.\n"
	  "  --role R      master (default; WE drive the handshake + own the clock — a box\n"
	  "                slaves to us) | slave (an external master drives; we lock to its\n"
	  "                cadence + return our inputs upstream)\n"
	  "  --rate R      the REAC sample rate: 44100, 48000 or 96000.\n"
	  "                Default 96000 in the MASTER role (a master DEFINES the rate;\n"
	  "                there is nothing to detect on a segment nobody is driving).\n"
	  "                As a SLAVE, auto-detected from the wire cadence. Given on the\n"
	  "                command line, it is a WHOLE-INVOCATION override and applies to\n"
	  "                every segment (auto-spine: this is the layer openmixer uses).\n"
	  "  --tx IFNAME   the REAC TX NIC: master role -> the reac:playback downstream sink;\n"
	  "                slave role -> the upstream return + handshake socket\n"
	  "  --box-channels N  SLAVE role: OUR OWN input width — what we declare as a box,\n"
	  "                which no wire can tell us (even 2..40; 8=S-0808, 16=S-1608,\n"
	  "                32=S-4000S). Default 16. Sets the cold-connect/upstream/heartbeat width.\n"
	  "  --mixer M     master role: which desk GENERATION to speak as (m200|m300|m5000;\n"
	  "                default m200). Sets the console-model byte only; the grants are\n"
	  "                box-defined so any box locks to any profile.\n"
	  "  (no --box)    master role: the box on this segment is LEARNED FROM THE WIRE and\n"
	  "                nothing else. reac-pw starts with no box, probes, and sizes +\n"
	  "                labels reac:capture / reac:playback the moment a box declares\n"
	  "                itself; a box swapped later re-derives everything. --box is\n"
	  "                RETIRED: it is accepted, ignored, and reported once.\n"
	  "  --name NAME   per-instance PipeWire node suffix (reac-capture.NAME /\n"
	  "                reac-playback.NAME) so one master per REAC VLAN/segment coexists.\n"
	  "  --headamp CH:PARAM:VALUE  master role, repeatable: a per-channel head-amp\n"
	  "                command the master re-asserts to the box (declarative/DMX).\n"
	  "                CH = head-amp channel 0..%d; PARAM = phantom|pad|sens; VALUE = 0/1\n"
	  "                for phantom|pad, 0..%d raw SENS code for sens. RIG-GATED.\n"
	  "  --src-mac M   our on-wire source MAC (aa:bb:cc:dd:ee:ff). Default for BOTH\n"
	  "                roles: the --tx NIC's OWN hardware address, verbatim — our frames\n"
	  "                carry OUR identity (real boxes and desks sync to it; a borrowed\n"
	  "                MAC collides with the real device and makes captures ambiguous).\n"
	  "no --live and no --pcap: the packaged-service shape. The daemon HEARS its segments:\n"
	  "  every Ethernet interface with link is sniffed (a passive 0x8819 socket), the first\n"
	  "  REAC frame heard makes that interface a segment named after it, and link loss\n"
	  "  drops it after a %d s hold. Nothing names an interface in advance.\n"
	  "auto-spine (ONE daemon, N listeners — 2026-08-20-reac-auto-spine.md §5): only the\n"
	  "  FIRST --live segment honours the per-box flags above. Every OTHER segment, and\n"
	  "  every heard one, reads its own settings from the layered conf, keyed by its name:\n"
	  "  REAC_<KEY>_<segment> in ~/.config/reac-pw/reac-pw.env above the bare REAC_<KEY>\n"
	  "  (reac_conf.h's precedence):\n"
	  "    REAC_TX=IFNAME             default: the same interface (this rig's masters\n"
	  "                               always tx == live)\n"
	  "    REAC_ROLE=master|slave     default: master\n"
	  "    REAC_MIXER=m200|m300|m5000 default: m200\n"
	  "    REAC_NAME=NAME             node suffix; default: the interface name (the FIRST\n"
	  "                               --live segment defaults to bare names instead,\n"
	  "                               matching every invocation before this one)\n"
	  "    REAC_HEADAMP=\"CH:PARAM:VALUE ...\"  the head-amp re-assertion table, space or\n"
	  "                               comma separated (replaces N --headamp flags)\n"
	  "    REAC_BOX_CHANNELS=N        slave role: our own input width; default 16\n"
	  "  and REAC_RATE per segment exactly as a single-segment run already resolves it.\n"
	  "environment (see docs/ENV-KNOBS.md; unset = default behavior, byte-identical):\n"
	  "  REACPW_GRANT_ON_DECLARE=0  master role: opt OUT of ending the grant dwell on the\n"
	  "                box's declaration, restoring the full wall-clock hold. The dwell is\n"
	  "                a CAP for an undeclared box, not a wait (default: end on declare).\n"
	  "  REACPW_GRANT_DWELL_S=N  master role: set that CAP to N whole seconds (default:\n"
	  "                the built-in ~1.6 s; a real M-200 holds a cold box ~27 s)\n"
	  "  REAC_DEBUG=1  opt-in RX/source telemetry on stderr (~every 2 s: frame/dup/gap\n"
	  "                counters, ring fill, active channels)\n"
	  "  REACPW_NO_ENROLL=1  master role: suppress the pre-grant ENROLL for a box whose\n"
	  "                width is already known (no real desk sends it to an S-1608). A\n"
	  "                RIG-TEST SWITCH, not a new default — see docs/ENV-KNOBS.md.\n"
	  "  REACPW_CLOCK_FOLLOW=1  master role: DISCIPLINE the TX cadence to the best\n"
	  "                available clock reference (NIC/external PHC > a hardware-driven\n"
	  "                PipeWire graph clock > the box's counter slope) instead of\n"
	  "                free-running on CLOCK_MONOTONIC. The period is steered\n"
	  "                continuously and bounded; the phase is never stepped. The\n"
	  "                reference in use is printed on every change, and with none\n"
	  "                available we free-run and SAY so. Unset = today's behaviour,\n"
	  "                byte- and timing-identical. RIG-GATED.\n"
	  "  REACPW_CLOCK_REF=<substring>  designate WHICH device is the clock reference\n"
	  "                (case-insensitive substring of the device name, e.g. 'Babyface').\n"
	  "                A designated device outranks the name heuristic; it does NOT\n"
	  "                rescue a structurally unusable one (HDMI/DisplayPort sinks,\n"
	  "                software timers) and it does NOT outrank measured instability.\n"
	  "                Only consulted when REACPW_CLOCK_FOLLOW is set.\n"
	  "  REACPW_CATCHUP_MAX_SLOTS=<n>  master role: how many OVERSLEPT slots the\n"
	  "                pacer repays by staying on its deadline grid instead of\n"
	  "                re-basing the phase and losing them. Unset = 4 (measured);\n"
	  "                -1 = never repay, the pre-2026-08-23 behaviour.\n"
	  "  REACPW_RATE_MATCH=1  master role: OPT IN to publishing io_rate_match on\n"
	  "                the sink, so PipeWire's resampler absorbs the residual\n"
	  "                graph/wire difference instead of the depth guard discarding\n"
	  "                it. Default OFF: the loop's sign is verified but its\n"
	  "                measurement phase is not, so it spends most of its\n"
	  "                authority on a standing correction. See ENV-KNOBS.md.\n"
	  "  REACPW_RT_PRIO=<1..99>  SCHED_FIFO priority for the wire-clock threads\n"
	  "                (the master pacer, the slave upstream engine). Unset = the\n"
	  "                built-in, which sits BELOW the PipeWire graph on purpose: a\n"
	  "                wire clock that outranks the audio driver preempts the cycle\n"
	  "                that fills its own ring, and the xruns land on the audio\n"
	  "                interface, not here. Raise it only on a host whose graph runs\n"
	  "                somewhere else; see src/reac_rt.h for the ladder.\n",
	  p, REAC_HEADAMP_MAX_CH - 1, REAC_HEADAMP_SENS_MAX,
	  (int)(REAC_IFSCAN_DOWN_HOLD_NS / 1000000000ULL));
}

/* MASTER autodetect — the only mode there is. A main-loop watcher that polls the box
 * the pacer recognized on the wire and (re)sizes the reac-capture / reac-playback
 * nodes to its real widths. Starting with NO BOX PRESENT is the normal state: nothing
 * plugged, nothing in the graph, and the nodes appear sized to the box the moment it
 * declares itself. Node create/destroy MUST run on the main/loop thread; recognition runs
 * in the RT pacer thread, which hands the model over via the pacer's atomic
 * recognized_box (read here through reac_sink_node_recognized_box) — so no pw_* call is
 * ever made from the pacer thread. The library owns the node lifecycle: this reads the
 * width + label and calls the two ensure() entry points, nothing more.
 *
 * One instance PER LISTENER (auto-spine §5): `tag` identifies which segment a line is
 * about once more than one shares this process's stderr, and is the empty string for a
 * lone listener — the historical, unprefixed output. */
struct autodetect_ctx {
	struct reac_source_node    **src;   /* main's source slot (created/rebuilt here) */
	struct reac_sink_node       *sink;  /* the master engine (owns the recognizer)   */
	struct reac_source_node_cfg  scfg;  /* stable reac-capture create args           */
	const struct reac_box_model *last;  /* last model acted on (edge-detects changes) */
	const char                  *pin;   /* a retired --box value, for the disagreement
	                                     * notice; NULL once reported (report ONCE)  */
	const char                  *tag;   /* "[iface] " once N>1, "" for a lone listener */
};

static void on_autodetect_timer(void *data, uint64_t expirations)
{
	(void)expirations;
	struct autodetect_ctx *c = data;
	const struct reac_box_model *bm = reac_sink_node_recognized_box(c->sink);
	if (!bm || bm == c->last)
		return;   /* nothing recognized yet, or the same model as last poll */
	c->last = bm;
	/* A pin that DISAGREES with the wire, said ONCE and never again. Once per frame is
	 * how a disagreement becomes wallpaper; never saying it is what lets a wrong pin sit
	 * in a unit file unnoticed for months. The wire has already won by the time this
	 * prints — it changes nothing, it only tells the operator which typed line is now
	 * describing a box that is not there. */
	{
		const char *pin = c->pin;   /* the notice CONSUMES c->pin; keep it to print */
		if (reac_box_pin_notice(&c->pin, bm->token))
			fprintf(stderr, "reac-pw: %s--box pinned '%.*s', the wire says %s — the "
			        "WIRE WINS and the nodes are now sized to it. The pin is still what "
			        "this segment shows before a box is powered, so fix it if this box "
			        "is the permanent one.\n",
			        c->tag, (int)strcspn(pin, ":"), pin, bm->display);
	}
	/* Everything derived from the recognized in_ch/out_ch — no per-model branches. */
	if (reac_source_node_ensure(c->src, &c->scfg, bm->in_ch, bm->display) != 0)
		fprintf(stderr, "reac-pw: %scould not size reac-capture to %d ch (%s)\n",
		        c->tag, bm->in_ch, bm->display);
	if (reac_sink_node_ensure(c->sink, bm->out_ch, bm->display) != 0)
		fprintf(stderr, "reac-pw: %scould not size reac-playback to %d ch (%s)\n",
		        c->tag, bm->out_ch, bm->display);
	fprintf(stderr, "reac-pw: %sautodetected %s -> reac-capture %d in / reac-playback "
	        "%d out\n", c->tag, bm->display, bm->in_ch, bm->out_ch);
}

/* ---- one listener per segment (auto-spine §5) ----------------------------
 *
 * Everything main() used to hold as single local variables — the RX feeder,
 * the ring, the source/sink nodes, the slave engine, the segment lock, the
 * autodetect watcher — lifted verbatim into one struct so main() can hold an
 * ARRAY of them and open each against the SAME shared PipeWire loop. Nothing
 * about a single segment's own behaviour changes; this is the per-segment
 * body multiplied, not redesigned. */

struct listener_cfg {
	struct reac_rx_cfg rxcfg;
	char tx_if_buf[64];   /* generous over IFNAMSIZ: silences -Wformat-truncation against the 256-byte conf value buffer */
	const char *tx_if;                 /* NULL = no TX side (RX-only monitor) */
	enum reac_role role;
	const struct reac_mixer_profile *mixer;
	char name_buf[64];    /* same reasoning as tx_if_buf */
	const char *inst_name;              /* NULL = bare node names */
	uint8_t src_mac[6];
	int src_mac_set;
	int box_channels;                   /* SLAVE role: our own input width */
	const struct reac_box_model *pin_model;
	const char *pin_label;
	const char *box_pin_spec;
	struct reac_headamp_setting headamps[REAC_HEADAMP_MAX_CH * REAC_HEADAMP_NPARAMS];
	int n_headamps;
	enum reac_conf_layer rate_layer;    /* ARGV when a whole-invocation --rate forced it */
	char tag[IFNAMSIZ + 4];             /* "[iface] " once N>1, "" for a lone listener */
	char iface_buf[IFNAMSIZ];           /* a heard segment's own interface name: rxcfg.source
	                                     * points here, because the table it was heard from
	                                     * reuses its slots */
};

struct listener {
	struct listener_cfg cfg;
	int opened;             /* listener_open() succeeded */
	int rx_started;         /* reac_rx_start() succeeded */

	struct reac_ring ring;
	struct reac_rx rx;

	struct reac_source_node *src;
	struct reac_source_node_cfg src_cfg;

	struct reac_sink_node *sink;
	struct reac_ring tx_ring;
	int tx_ring_init;

	struct reac_slave slave;
	int slave_open;

	struct reac_seglock seglock;

	/* THE SEGMENT'S ROLE LIFECYCLE, and the reason it lives HERE. A role change
	 * closes one engine and opens the other, so every per-engine home for this
	 * record is destroyed halfway through the answer it owes. The listener is
	 * what a segment IS across both engines; the record rides it, the nodes
	 * borrow it to publish, and reac_role_swap_state derives the answer from it
	 * (reac_role_swap.h). MAIN-LOOP-ONLY, like every writer that touches it. */
	struct reac_role_swap role_swap;

	/* IS A MASTER STILL BEING HEARD? The slave role's half of the segment
	 * aggregate, latched here because it is a fact about the SEGMENT over time
	 * and the RX only counts frames (reac_segment_ident.h). Stepped from the same
	 * 200 ms poll that publishes, so the claim decays when a desk is unplugged
	 * instead of standing on a counter that never goes back down. */
	struct reac_segment_heard heard;

	struct autodetect_ctx adc;
	struct spa_source *ad_timer;
};

static void listener_cfg_defaults(struct listener_cfg *c)
{
	memset(c, 0, sizeof *c);
	c->rxcfg.pcap_realtime = 1;
	c->role = REAC_ROLE_MASTER;
	c->mixer = reac_mixer_profile_by_name("m200");
	c->box_channels = REAC_SLAVE_BOX_CHANNELS_DEFAULT;
}

/* Fill a listener's configuration from the layered conf files, keyed by ITS
 * OWN interface — the shape a segment gets when it reaches main() with no
 * per-box command-line flags at all (every segment beyond the first, and the
 * packaged service's segments, which get none). `is_first` controls only the
 * node-name default: the FIRST segment keeps BARE names (no REAC_NAME -> NULL,
 * matching every invocation before this one, where an unnamed master gets
 * unnamed nodes); every OTHER segment defaults its name to its own interface,
 * so two segments never collide on "reac-capture"/"reac-playback" with
 * nothing in the conf to tell them apart. */
static void listener_cfg_from_conf(struct listener_cfg *c, const char *iface, int is_first)
{
	listener_cfg_defaults(c);
	c->rxcfg.kind = REAC_RX_LIVE;
	snprintf(c->iface_buf, sizeof c->iface_buf, "%s", iface);
	c->rxcfg.source = c->iface_buf;
	iface = c->iface_buf;

	char v[256];

	if (reac_conf_lookup("REAC_TX", iface, NULL, v, sizeof v) != REAC_CONF_NONE)
		snprintf(c->tx_if_buf, sizeof c->tx_if_buf, "%.63s", v);
	else
		snprintf(c->tx_if_buf, sizeof c->tx_if_buf, "%s", iface);
	c->tx_if = c->tx_if_buf;

	if (reac_conf_lookup("REAC_ROLE", iface, NULL, v, sizeof v) != REAC_CONF_NONE) {
		enum reac_role r;
		if (reac_role_parse(v, &r) == 0)
			c->role = r;
		else
			fprintf(stderr, "reac-pw: [%s] ignoring REAC_ROLE='%s' (master|slave)\n",
			        iface, v);
	}

	if (reac_conf_lookup("REAC_MIXER", iface, NULL, v, sizeof v) != REAC_CONF_NONE) {
		const struct reac_mixer_profile *m = reac_mixer_profile_by_name(v);
		if (m)
			c->mixer = m;
		else
			fprintf(stderr, "reac-pw: [%s] ignoring unknown REAC_MIXER='%s'\n", iface, v);
	}

	if (reac_conf_lookup("REAC_NAME", iface, NULL, v, sizeof v) != REAC_CONF_NONE) {
		snprintf(c->name_buf, sizeof c->name_buf, "%.63s", v);
		c->inst_name = c->name_buf;
	} else if (!is_first) {
		snprintf(c->name_buf, sizeof c->name_buf, "%s", iface);
		c->inst_name = c->name_buf;
	}
	/* is_first with no REAC_NAME: inst_name stays NULL (bare names). */

	if (reac_conf_lookup("REAC_HEADAMP", iface, NULL, v, sizeof v) != REAC_CONF_NONE) {
		int n = parse_headamp_list(v, c->headamps,
		                          (int)(sizeof c->headamps / sizeof c->headamps[0]));
		if (n < 0)
			fprintf(stderr, "reac-pw: [%s] ignoring malformed REAC_HEADAMP\n", iface);
		else
			c->n_headamps = n;
	}

	if (reac_conf_lookup("REAC_BOX_CHANNELS", iface, NULL, v, sizeof v) != REAC_CONF_NONE) {
		int n = atoi(v);
		if (n >= 2 && n <= REAC_MAX_CHANNELS && (n & 1) == 0)
			c->box_channels = n;
		else
			fprintf(stderr, "reac-pw: [%s] ignoring invalid REAC_BOX_CHANNELS='%s'\n",
			        iface, v);
	}

	/* REAC_RATE is resolved inside listener_open, by the SAME per-segment
	 * logic every listener uses — no separate copy of that lookup here. */
}

/* The master-role rate default, resolved exactly as a single-instance run
 * always has (docs/RATE-AND-CLOCK-CONFIG.md): an explicit --rate (ARGV,
 * already stored in c->rxcfg.forced_rate before this is called) wins
 * outright; otherwise the layered conf is consulted FOR THIS SEGMENT; the
 * built-in best-drivable pick is the floor. */
static int listener_resolve_rate(const struct listener_cfg *c, enum reac_conf_layer *out_layer)
{
	char v[64];
	const char *seg = (c->rxcfg.kind == REAC_RX_LIVE) ? c->rxcfg.source : NULL;
	enum reac_conf_layer got = reac_conf_lookup("REAC_RATE", seg, NULL, v, sizeof v);
	if (got != REAC_CONF_NONE) {
		int r = atoi(v);
		if (r == 44100 || r == 48000 || r == 96000) {
			*out_layer = got;
			return r;
		}
		/* A layer that answered with nonsense must SAY so and be skipped, not
		 * silently drop us to the built-in with no explanation. */
		fprintf(stderr, "reac-pw: %signoring REAC_RATE='%s' from %s — REAC "
		        "runs at 44100, 48000 or 96000 Hz and nothing else\n",
		        c->tag, v, reac_conf_layer_name(got));
	}
	*out_layer = REAC_CONF_BUILTIN;
	return reac_rate_best_drivable(REAC_RATE_ALL_BITS);
}

/* Publish the SEGMENT'S WHOLE ANSWER on the node that exists in the role it is
 * running: the master's reac-playback carries it from the sink's own badge
 * timer, so this is the SLAVE half — its reac-capture node is the only node a
 * recorder has, and therefore both its door and its voice.
 *
 * PARITY IS THE POINT (docs/SLAVE-EMULATION-SCOPE.md W1). A recorder used to
 * publish the role trio and nothing else, so the same segment answered a console
 * richly as a mixer and almost not at all as a recorder — which is why the role
 * round trip only went one way. It now answers with the identity
 * (stamped at create, reac_source_node.c) plus the reac.master.* aggregate and
 * the wire pace, so a segment is the same addressable fact in either role.
 *
 * Everything here is DERIVED on every call — the role from the segment's record
 * plus the slave engine's own ESTABLISHED flag, the aggregate from the RX's
 * accepted-frame evidence — so a recorder hunting a desk that is not there says
 * `role_hunting` and `master.state=none` for as long as both are true, and a desk
 * that is unplugged stops being reported rather than standing on a stale count.
 *
 * A slave never emits head-amp (a box is told what its preamps do, it does not
 * tell its desk), which is true by construction — reac_slave's emit vocabulary
 * has no head-amp member — and asserted here so the property is re-checked on
 * the side the swap just moved to. */
static void listener_publish_segment(struct listener *L)
{
	if (!L->src)
		return;
	int established = L->slave_open
		? atomic_load_explicit(&L->slave.established, memory_order_relaxed) : 0;
	const char *state = reac_role_swap_state(
		&L->role_swap, reac_role_engine_of_slave(L->slave_open, established));
	char role_s[4];
	snprintf(role_s, sizeof role_s, "%d", REAC_CFG_ROLE_VALUE_SLAVE);

	/* THE SEGMENT AGGREGATE, from what this engine actually witnessed. The
	 * evidence is the RX's accepted-frame count: in the slave role the gate
	 * passes the 40-channel master downstream and nothing else, so a moving
	 * count is both "a master is here" and "its geometry is a desk's" — the same
	 * two facts a master's arbitration derives from its sighting table, and
	 * derived here through the SAME classifier rather than a second one. */
	int heard = reac_segment_heard_step(
		&L->heard,
		L->rx_started ? atomic_load_explicit(&L->rx.frames_ok, memory_order_relaxed) : 0,
		REAC_SEGMENT_HEARD_QUIET_TICKS);
	struct reac_segment_answer answer;
	reac_segment_answer_slave(&answer, heard,
	                          L->slave_open
	                            ? atomic_load_explicit(&L->slave.master_mac48,
	                                                   memory_order_relaxed) : 0,
	                          L->rx.sample_rate);

	reac_source_node_publish_segment(L->src, role_s, state,
	                                 reac_role_refuse_code(REAC_ROLE_REFUSE_NONE),
	                                 &answer);
}

/* Bring one segment online: resolve its rate, open the RX feeder, and (role
 * permitting) the TX side + node lifecycle — the single-instance body main()
 * used to run inline, called once per configured interface against ONE
 * shared loop. Returns 0 with L fully populated (still needs
 * reac_rx_start()), or -1 on a refusal this segment cannot recover from
 * (already reported on stderr, and everything this call opened is already
 * cleaned up). A refusal here does not necessarily end the daemon — see
 * main()'s single-vs-multi distinction at the call site. */
static int listener_open(struct listener *L, struct pw_loop *loop)
{
	struct listener_cfg *c = &L->cfg;

	L->src = NULL;
	L->sink = NULL;
	L->slave_open = 0;
	L->tx_ring_init = 0;
	L->seglock.fd = -1;
	L->ad_timer = NULL;

	/* SAMPLE RATE — the master chooses it; the box follows. See
	 * docs/RATE-AND-CLOCK-CONFIG.md for the full law; this is its per-segment
	 * application, unchanged from the single-instance code it replaces. */
	if (c->role == REAC_ROLE_MASTER && c->rxcfg.forced_rate == 0)
		c->rxcfg.forced_rate = listener_resolve_rate(c, &c->rate_layer);
	else if (c->rxcfg.forced_rate != 0)
		c->rate_layer = REAC_CONF_ARGV;

	/* NAME THE LAYER THAT ANSWERED, not merely the value — three sources have
	 * disagreed on this rig at once and the disagreement was invisible
	 * because the startup line said only the number. */
	fprintf(stderr, "reac-pw: %sREAC rate = %d Hz (%d pps), from %s\n", c->tag,
	        c->rxcfg.forced_rate, c->rxcfg.forced_rate / REAC_SAMPLES_PER_PKT,
	        c->rxcfg.forced_rate == 0 ? "auto-detect from the wire cadence"
	                                  : reac_conf_layer_name(c->rate_layer));

	/* The role picks which stream RX decodes (see DESIGN's role table): as
	 * MASTER our capture is a box's upstream return (its input channels,
	 * box-width braided frames); as SLAVE it is the master's 40-ch downstream
	 * broadcast. The wire carries both; the gate keeps them apart. */
	c->rxcfg.accept = (c->role == REAC_ROLE_MASTER) ? REAC_RX_ACCEPT_UPSTREAM
	                                                : REAC_RX_ACCEPT_DOWNSTREAM;

	if (reac_rx_open(&L->rx, &c->rxcfg, &L->ring) != 0) {
		fprintf(stderr, "reac-pw: %scannot open source '%s'\n", c->tag, c->rxcfg.source);
		return -1;
	}
	fprintf(stderr, "reac-pw: %srecovered REAC rate = %d Hz (%d pps), rx stream = %s\n",
	        c->tag, L->rx.sample_rate, L->rx.sample_rate / REAC_SAMPLES_PER_PKT,
	        c->rxcfg.accept == REAC_RX_ACCEPT_UPSTREAM
	          ? "box upstream return (box-width)" : "master downstream (40 ch)");

	/* The reac-capture source is created AFTER the TX side, because whether to DEFER
	 * it depends on whether a recognizer (the master pacer) exists. In pure autodetect
	 * (master + a live TX pacer) it is deferred: nothing plugged -> nothing in the
	 * graph, and the node appears sized to the box the moment it is recognized (the
	 * autodetect timer below). Every other mode (slave, or pcap / no-TX master) has no
	 * recognizer, so the node is created at its startup width. */
	L->src_cfg = (struct reac_source_node_cfg){
		.loop = loop, .ring = &L->ring, .rx = &L->rx, .sample_rate = L->rx.sample_rate,
		.inst = c->inst_name, .master_role = (c->role == REAC_ROLE_MASTER),
	};

	/* TX side: who drives the handshake + the clock depends on the role.
	 *   master -> reac:playback sink: WE encode the graph downstream + the pacer
	 *             drives the cdea/cfea grant + owns the clock (a box slaves to us).
	 *   slave  -> reac_slave engine: an external master drives; we lock to its
	 *             cadence + return our input channels upstream at the box's slots. */
	if (c->tx_if && c->role == REAC_ROLE_MASTER) {
		/* Default master MAC = THIS NIC's own address (reac_mac.h); --src-mac
		 * overrides it. The mixer profile sets only the console-model byte. */
		uint8_t master_mac_buf[6];
		if (!c->src_mac_set && reac_mac_default_src(c->tx_if, master_mac_buf) != 0)
			fprintf(stderr, "reac-pw: %scould not read %s hardware address for the "
			        "master source MAC; using the locally-administered fallback\n",
			        c->tag, c->tx_if);
		const uint8_t *master_src = c->src_mac_set ? c->src_mac : master_mac_buf;
		reac_ring_init(&L->tx_ring, REAC_MAX_CHANNELS, (uint32_t)(L->rx.sample_rate / 4));
		L->tx_ring_init = 1;
		struct reac_sink_cfg scfg = { .ifname = c->tx_if,
		                              /* The graph filter is DEFERRED until a box is
		                               * recognized (reac_sink_node_new leaves it at 0
		                               * and the autodetect watcher sizes it), so this
		                               * is only the ceiling. */
		                              .channels = REAC_MAX_CHANNELS,
		                              .sample_rate = L->rx.sample_rate,
		                              .src_mac = master_src, .master_mac = NULL,
		                              /* THE FAMILY, as configured. It does NOT come from the
	                               * rate: the two are independent settings and each is
	                               * obeyed as given (see reac_master.h). */
	                              .console_field = c->mixer->console_field,
		                              .inst = c->inst_name, .label = NULL,
		                              .headamps = c->n_headamps ? c->headamps : NULL,
		                              .n_headamps = c->n_headamps,
		                              /* #75: default OFF -> the pacer free-runs on
		                               * CLOCK_MONOTONIC exactly as it always has. */
		                              .clock_follow = getenv("REACPW_CLOCK_FOLLOW") != NULL,
		                              /* --rate / a conf-file rate is an ASSERTION; only the
		                               * built-in best-drivable pick is the convention. */
		                              .rate_asserted = c->rate_layer != REAC_CONF_BUILTIN
		                                            && c->rate_layer != REAC_CONF_NONE,
		                              /* #77: unset -> nothing is designated and the
		                               * name heuristic alone grades the reference. */
		                              .clock_ref = getenv("REACPW_CLOCK_REF"),
		                              /* Slot-debt budget. Unset -> the measured
		                               * default; see reac_pacer.h. */
		                              .catchup_max_slots = getenv("REACPW_CATCHUP_MAX_SLOTS")
		                                  ? atoi(getenv("REACPW_CATCHUP_MAX_SLOTS"))
		                                  : 0,
		                              /* RATE MATCHING SHIPS OFF. OPT IN WITH
		                               * REACPW_RATE_MATCH=1. See reac_pacer.h's
		                               * cfg.rate_match_off for the full measurement —
		                               * unchanged by this refactor. */
		                              .rate_match_off =
		                                  (getenv("REACPW_RATE_MATCH") &&
		                                   atoi(getenv("REACPW_RATE_MATCH")) != 0)
		                                  ? 0 : -1 };
		/* CLAIM THE SEGMENT BEFORE THE FIRST FRAME. Driving is what takes the
		 * lock; RX above has been running unlocked, which is correct — observing a
		 * segment is a copy and must stay safe beside somebody else's master. */
		int claimed = reac_seglock_claim(&L->seglock, c->tx_if);
		if (claimed == -1) {
			fprintf(stderr,
			    "reac-pw: %sREFUSING to master '%s' — another process already holds\n"
			    "         that segment (%s). Two masters on one segment is the\n"
			    "         fault this lock exists to make impossible; it has cost an\n"
			    "         evening once and corrupted a live measurement once.\n"
			    "         Nothing is taken over automatically: stop the holder, or\n"
			    "         drive a different segment. Who holds it:\n"
			    "           grep %s /proc/net/unix\n",
			    c->tag, c->tx_if, L->seglock.name, L->seglock.name);
			reac_rx_close(&L->rx);
			reac_ring_free(&L->ring);
			reac_ring_free(&L->tx_ring);
			return -1;
		}
		if (claimed == -2)
			fprintf(stderr, "reac-pw: %scould not claim a segment lock for '%s' "
			        "(interface or netns unreadable); proceeding UNPROTECTED\n",
			        c->tag, c->tx_if);

		L->sink = reac_sink_node_new(loop, &L->tx_ring, &scfg); /* encodes + emits REAC */
		if (!L->sink) {
			/* A master with no TX is not a degraded master, it is a silent one:
			 * it probes nothing, grants nothing and syncs no box, while every
			 * other sign of health stays green. Refuse instead. */
			explain_tx_failure(c->tx_if);
			reac_seglock_release(&L->seglock);
			reac_rx_close(&L->rx);
			reac_ring_free(&L->ring);
			reac_ring_free(&L->tx_ring);
			return -1;
		}
		fprintf(stderr, "reac-pw: %sMASTER role (%s profile) on '%s' — "
		        "event-driven establishment: probing until the box's "
		        "cold-connect (cdea 04 03) arrives; FSM/RX transcript on "
		        "stderr\n", c->tag, c->mixer->display, c->tx_if);
		if (c->n_headamps)
			fprintf(stderr, "reac-pw: %shead-amp DMX send armed — %d cell(s), "
			        "re-asserted once established (RIG-GATED: verify 48V at the "
			        "XLR pins)\n", c->tag, c->n_headamps);
	} else if (c->tx_if && c->role == REAC_ROLE_SLAVE) {
		/* The slave returns its OWN input channels (a box width) upstream. The PCM
		 * for them would come from a reac:return sink; for now the ring is the
		 * carrier and the slave emits silent/own-input FILLER until that sink is
		 * linked. The engine learns the master MAC from the wire — never set here. */
		uint8_t box_mac[6];
		if (c->src_mac_set) {
			memcpy(box_mac, c->src_mac, 6);
		} else if (reac_mac_default_src(c->tx_if, box_mac) != 0) {
			/* NIC hwaddr unreadable — the locally-administered fallback is still
			 * on-wire safe (no manufacturer carries it), but note it so an
			 * ambiguous capture is explained. */
			fprintf(stderr, "reac-pw: %scould not read %s hardware address for the box "
			        "source MAC; using the locally-administered fallback\n",
			        c->tag, c->tx_if);
		}
		fprintf(stderr, "reac-pw: %sslave box source MAC = "
		        "%02x:%02x:%02x:%02x:%02x:%02x%s\n", c->tag,
		        box_mac[0], box_mac[1], box_mac[2], box_mac[3], box_mac[4], box_mac[5],
		        c->src_mac_set ? " (--src-mac override)"
		                       : " (this NIC's own address; --src-mac overrides)");
		reac_ring_init(&L->tx_ring, REAC_MAX_CHANNELS, (uint32_t)(L->rx.sample_rate / 4));
		L->tx_ring_init = 1;
		struct reac_slave_cfg slcfg = { .ifname = c->tx_if,
		                                .box_channels = c->box_channels,
		                                .sample_rate = L->rx.sample_rate,
		                                .src_mac = box_mac };
		if (reac_slave_open(&L->slave, &slcfg, &L->tx_ring) == 0) {
			L->slave_open = 1;
			if (reac_slave_start(&L->slave) == 0) {
				reac_slave_set_phy_up(&L->slave, 1);  /* PHY up: begin the establishment */
				fprintf(stderr, "reac-pw: %sSLAVE role (%d-ch upstream return) — "
				        "responding to an external master, locked to its cadence\n",
				        c->tag, c->box_channels);
			} else {
				fprintf(stderr, "reac-pw: %sslave engine thread failed to start\n", c->tag);
				reac_slave_close(&L->slave); L->slave_open = 0;
			}
		} else {
			fprintf(stderr, "reac-pw: %sslave engine not created (AF_PACKET on '%s' "
			        "failed — need CAP_NET_RAW?)\n", c->tag, c->tx_if);
		}
	}

	/* Now that we know whether a recognizer exists (master + a live TX pacer), either
	 * DEFER the box nodes to autodetect or expose the source at its startup width. */
	L->adc = (struct autodetect_ctx){0};
	/* #75: the RX feeder is the BOX clock reference's measurement source — it already
	 * tracks the box's counter slope and publishes a filtered ppm error. The sink's
	 * existing 200 ms timer forwards it to the pacer's discipline. Wired
	 * unconditionally; it is only ever read when clock following is enabled. */
	if (L->sink)
		reac_sink_node_set_rate_source(L->sink, &L->rx);
	/* The master node publishes reac.cfg.role.state off the SEGMENT's record, so
	 * the answer is derived from the engine that is actually up rather than
	 * frozen at the moment an assertion was parsed. */
	if (L->sink)
		reac_sink_node_set_role_swap(L->sink, &L->role_swap);
	if (c->role == REAC_ROLE_MASTER && L->sink) {
		/* Pure autodetect: the pacer recognizes the box on the wire; a 200 ms main-
		 * loop watcher then (re)sizes reac-capture / reac-playback to its widths. No
		 * box node exists until then (nothing plugged = nothing in the graph). */
		L->adc.src = &L->src;
		L->adc.sink = L->sink;
		L->adc.scfg = L->src_cfg;
		L->adc.pin  = c->box_pin_spec;      /* reported once, if the wire disagrees */
		L->adc.tag  = c->tag;
		/* #208: let the sink's badge timer keep the reac-capture node's link-state /
		 * box-model / box-width in sync (it has no pacer handle of its own). Same source
		 * slot the autodetect watcher rebuilds, so a live box-width change is followed. */
		reac_sink_node_set_peer_source(L->sink, &L->src);
		L->ad_timer = pw_loop_add_timer(loop, on_autodetect_timer, &L->adc);
		if (L->ad_timer) {
			struct timespec first = { 0, 200 * 1000000L };
			struct timespec interval = { 0, 200 * 1000000L };
			pw_loop_update_timer(loop, L->ad_timer, &first, &interval, false);
		}
		if (c->pin_model) {
			/* The fixed-install pin: put the nodes on the graph NOW, at the pinned
			 * width and name, so the patch exists before the box is powered. This
			 * is the same pair of calls the autodetect watcher makes on
			 * recognition, so a box that later declares something else simply
			 * re-sizes them — no separate "pinned" code path to diverge. */
			if (reac_source_node_ensure(&L->src, &L->src_cfg, c->pin_model->in_ch, c->pin_label) != 0 ||
			    reac_sink_node_ensure(L->sink, c->pin_model->out_ch, c->pin_label) != 0) {
				fprintf(stderr, "reac-pw: %s--box: could not size the nodes to %s\n",
				        c->tag, c->pin_model->display);
				return -1;
			}
			fprintf(stderr, "reac-pw: %sMASTER pinned --box %s — reac-capture %d ch / "
			        "reac-playback %d ch labelled '%s', present from boot. The pin names "
			        "and sizes the ports; the WIRE still decides what is enrolled, and "
			        "outranks the pin if a different box declares itself.\n",
			        c->tag, c->pin_model->token, c->pin_model->in_ch, c->pin_model->out_ch,
			        c->pin_label);
		} else {
			fprintf(stderr, "reac-pw: %sMASTER autodetect — reac-capture / reac-playback "
			        "appear sized to the box once it is recognized on the wire\n", c->tag);
		}
	} else {
		/* No recognizer (slave, or pcap / no-TX master): expose the source now, at
		 * the full 40-slot fabric. With no recognizer there is nothing that could
		 * honestly narrow it to a box, and nothing may pretend otherwise. */
		if (reac_source_node_ensure(&L->src, &L->src_cfg, 0, NULL) != 0) {
			fprintf(stderr, "reac-pw: %sfailed to create reac:capture node\n", c->tag);
			return -1;
		}
	}

	/* THIS ROLE'S ENGINE NOW OWNS THE SEGMENT. Recorded on the only success
	 * path, so a refused open leaves the record saying the segment is down —
	 * which is what it is. */
	reac_role_swap_opened(&L->role_swap, c->role);
	/* The slave has no reac-playback node, so its capture node carries BOTH
	 * halves — the write door and the answer (see listener_publish_segment and
	 * reac_source_node.h). Wired in the slave role only: in the master role the
	 * sink owns them, and a second door onto the same fact is exactly what the
	 * one-store law refuses. */
	if (c->role == REAC_ROLE_SLAVE) {
		reac_source_node_set_role_swap(L->src, &L->role_swap);
		/* Fresh evidence for a fresh engine: a re-opened segment must not
		 * inherit the previous one's "a master was heard" (the RX and its
		 * counter are new here anyway, and seeding from 0 says so). */
		reac_segment_heard_init(&L->heard, 0);
		listener_publish_segment(L);
	}
	return 0;
}

/* Tear down one segment, mirroring main()'s single-instance shutdown block.
 * Safe on a partially-opened L (every destroy/close/free below is documented
 * NULL/unheld-safe), so it doubles as listener_open()'s own failure cleanup. */
static void listener_close(struct listener *L, struct pw_loop *loop)
{
	reac_rx_stop(&L->rx);
	if (L->ad_timer)
		pw_loop_destroy_source(loop, L->ad_timer);   /* stop the autodetect watcher first */
	reac_source_node_destroy(L->src);                /* may be NULL (never recognized) */
	reac_sink_node_destroy(L->sink);
	if (L->slave_open) {
		reac_slave_stop(&L->slave);
		reac_slave_close(&L->slave);
	}
	if (L->tx_ring_init)
		reac_ring_free(&L->tx_ring);
	reac_rx_close(&L->rx);
	reac_ring_free(&L->ring);
	reac_seglock_release(&L->seglock);
	/* Socket, thread and (for a master) the segment lock are gone: nothing owns
	 * this segment until the next open, and the role answer says exactly that. */
	reac_role_swap_closed(&L->role_swap);
}

/* --- clean segment re-open on a REAC rate change (2026-08-26) ----------------
 * The operator ruled re-establishment acceptable: a REAC pace change re-clocks
 * the segment and the box re-enrolls. We do it the fresh-launch way — tear the
 * segment down and bring it back up at the new rate — so the pacer, ring and
 * nodes are all clean, instead of the in-place reconnect that left the ring
 * running deep and dropping slots for ~1 min (2026-08-26-rate-change-jitter). */
static void listener_reopen_at_rate(struct listener *L, struct pw_loop *loop, int hz)
{
	fprintf(stderr, "reac-pw: %sREAC rate -> %d Hz: clean segment re-open (box re-enrolls)\n",
	        L->cfg.tag, hz);
	if (L->opened)
		listener_close(L, loop);
	L->opened = 0;
	L->rx_started = 0;
	L->cfg.rxcfg.forced_rate = hz;
	if (listener_open(L, loop) != 0) {
		fprintf(stderr, "reac-pw: %sre-open at %d Hz FAILED — segment down\n", L->cfg.tag, hz);
		return;
	}
	L->opened = 1;
	if (reac_rx_start(&L->rx) != 0) {
		fprintf(stderr, "reac-pw: %sre-open RX start FAILED — segment down\n", L->cfg.tag);
		listener_close(L, loop);
		L->opened = 0;
		return;
	}
	L->rx_started = 1;
}

/* A clean segment re-open in the OTHER engine (master<->slave) — the cross-engine
 * swap, on the rate re-open's own pattern (request atom -> main-loop drain ->
 * re-open) but strictly larger, because the two roles are two ENGINES rather
 * than one engine at a different cadence. listener_close takes down the running
 * engine's AF_PACKET socket, its thread (the SCHED_FIFO pacer for a master, the
 * emitter for a slave) and — master only — the segment lock; listener_open then
 * brings the other one up through the SAME seam a cold start uses, so nothing
 * about the new engine is a special swap path that a fresh launch does not
 * exercise.
 *
 * THE ANSWER NEVER RUNS AHEAD OF THE ENGINE. The record says the segment is down
 * from the moment the old engine closes, and `applied` is unreachable until the
 * NEW engine is doing its role's own job — for a master, pacing its wire; for a
 * slave, having been enrolled by a desk. A slave that comes up on a quiet wire
 * therefore answers `role_hunting` and keeps answering it (reac_role_swap.h).
 *
 * NOT CLAIMED HERE: that a real box or desk re-attaches across this. No capture
 * in the corpus shows a desk ceding a segment or a box under a master that
 * changes role, so that half is an operator-present rig test and nothing in this
 * file's transcript may be read as evidence for it. */
static void listener_reopen_at_role(struct listener *L, struct pw_loop *loop, enum reac_role role)
{
	fprintf(stderr, "reac-pw: %sREAC role -> %s: clean segment re-open (cross-engine swap; "
	        "head-amp is emitted by the %s engine only)\n",
	        L->cfg.tag, reac_role_name(role),
	        reac_role_emits_headamp(role) ? "running master" : "master — this one sends none");
	if (L->opened)
		listener_close(L, loop);
	L->opened = 0;
	L->rx_started = 0;
	L->cfg.role = role;
	if (listener_open(L, loop) != 0) {
		/* The record still says the segment is down, so the answer stays
		 * `role_reestablish_pending` — which is the truth: the role was
		 * accepted and nothing is performing it. */
		fprintf(stderr, "reac-pw: %srole re-open FAILED — segment down, role still owed\n",
		        L->cfg.tag);
		return;
	}
	L->opened = 1;
	if (reac_rx_start(&L->rx) != 0) {
		fprintf(stderr, "reac-pw: %srole re-open RX start FAILED — segment down\n", L->cfg.tag);
		listener_close(L, loop);
		L->opened = 0;
		return;
	}
	L->rx_started = 1;
	fprintf(stderr, "reac-pw: %s%s engine up; role state = %s\n", L->cfg.tag,
	        reac_role_name(role),
	        reac_role_swap_state(&L->role_swap,
	                             role == REAC_ROLE_MASTER
	                               ? REAC_ROLE_ENGINE_HUNTING   /* IDLE until the first frame */
	                               : reac_role_engine_of_slave(L->slave_open, 0)));
}

/* ---- HEARING: the segments are discovered, not declared ---------------------
 *
 * openmixer's 2026-08-23-reac-trunk-vlan-daemon.md §7-§9, amendment 2026-09-02.
 * reac_ifscan keeps the interface table and says what to do; this block owns
 * what the verbs refer to — one passive sniffer per linked interface, and the
 * listener slots a heard segment is served from — and runs on the main loop
 * only. Sniffers and the netlink watch only FEED the table from their io
 * callbacks; the table's events are applied from the 200 ms poll, so no
 * source is ever destroyed from inside its own callback. */

struct sniffer {
	char name[IFNAMSIZ];        /* "" = free slot */
	struct reac_capture cap;
	struct spa_source *io;
	uint8_t mac[6];             /* the NIC's own address: our echo, if any, is not a sighting */
	unsigned long frames;       /* 0x8819 frames read, whether or not they classified */
};

struct hearing {
	int enabled;
	struct reac_ifscan scan;
	struct spa_source *nl_io;
	struct sniffer sniff[REAC_IFSCAN_MAX];
	struct listener *listeners;
	int n_slots;
	struct pw_loop *loop;
	int forced_rate;            /* a whole-invocation --rate, applied to every heard segment */
	unsigned long served, dropped;
};

static struct hearing g_hear;

static uint64_t monotonic_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static struct sniffer *sniffer_find(struct hearing *h, const char *name)
{
	for (int i = 0; i < REAC_IFSCAN_MAX; i++)
		if (h->sniff[i].name[0] && strcmp(h->sniff[i].name, name) == 0)
			return &h->sniff[i];
	return NULL;
}

/* A sniffer's socket is readable: read it dry, classify, and tell the table
 * about the first frame that IS REAC gear. Never transmits, never touches a
 * listener. */
static void on_sniff_io(void *data, int fd, uint32_t mask)
{
	(void)fd;
	struct sniffer *sn = data;
	if (!(mask & SPA_IO_IN))
		return;
	uint8_t frame[2048];
	for (int i = 0; i < 64; i++) {
		long n = reac_capture_next(&sn->cap, frame, sizeof frame);
		if (n <= 0)
			break;
		sn->frames++;
		struct reac_disco_sighting sight;
		if (reac_disco_classify(frame, (size_t)n, sn->mac, &sight) != 0)
			continue;
		const struct reac_ifscan_entry *e = reac_ifscan_find(&g_hear.scan, sn->name);
		if (e && e->state == REAC_IFSCAN_LINKED &&
		    (e->retry_after_ns == 0 || monotonic_ns() >= e->retry_after_ns))
			fprintf(stderr, "reac-pw: [%s] REAC heard — %s %02x:%02x:%02x:%02x:%02x:%02x"
			        "%s%s (%u ch): this interface is a segment\n",
			        sn->name, reac_disco_role_name(sight.role),
			        sight.mac[0], sight.mac[1], sight.mac[2],
			        sight.mac[3], sight.mac[4], sight.mac[5],
			        sight.model ? " " : "", sight.model ? sight.model->display : "",
			        sight.channels);
		reac_ifscan_heard(&g_hear.scan, sn->name, monotonic_ns());
		break;
	}
}

static void sniffer_close(struct hearing *h, const char *name)
{
	struct sniffer *sn = sniffer_find(h, name);
	if (!sn)
		return;
	if (sn->io)
		pw_loop_destroy_source(h->loop, sn->io);
	reac_capture_close(&sn->cap);
	memset(sn, 0, sizeof *sn);
}

static int sniffer_open(struct hearing *h, const char *name)
{
	if (sniffer_find(h, name))
		return 0;
	struct sniffer *sn = NULL;
	for (int i = 0; i < REAC_IFSCAN_MAX; i++)
		if (!h->sniff[i].name[0]) { sn = &h->sniff[i]; break; }
	if (!sn)
		return -1;
	memset(sn, 0, sizeof *sn);
	if (reac_capture_open(&sn->cap, name) != 0) {
		fprintf(stderr, "reac-pw: [%s] link is up but the 0x8819 sniffer could not open: %s "
		        "— this interface is not watched\n", name, strerror(errno));
		memset(sn, 0, sizeof *sn);
		return -1;
	}
	reac_capture_set_nonblock(&sn->cap, 1);
	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
	if (ioctl(sn->cap.fd, SIOCGIFHWADDR, &ifr) == 0)
		memcpy(sn->mac, ifr.ifr_hwaddr.sa_data, 6);
	snprintf(sn->name, IFNAMSIZ, "%s", name);
	sn->io = pw_loop_add_io(h->loop, sn->cap.fd, SPA_IO_IN, false, on_sniff_io, sn);
	if (!sn->io) {
		reac_capture_close(&sn->cap);
		memset(sn, 0, sizeof *sn);
		return -1;
	}
	fprintf(stderr, "reac-pw: [%s] link up — listening for REAC (nothing transmitted "
	        "until something is heard)\n", name);
	return 0;
}

static struct listener *hearing_listener(struct hearing *h, const char *name)
{
	for (int i = 0; i < h->n_slots; i++) {
		struct listener *L = &h->listeners[i];
		if (L->opened && L->cfg.rxcfg.source && strcmp(L->cfg.rxcfg.source, name) == 0)
			return L;
	}
	return NULL;
}

/* Open the full listener on a heard segment: the same body every --live
 * segment gets, configured from the layered conf under the segment's own
 * name, with no first-is-bare exception — bare node names belong to the
 * --live dev shape alone, so two heard segments can never collide. */
static void hearing_serve(struct hearing *h, const char *name)
{
	struct listener *L = NULL;
	for (int i = 0; i < h->n_slots; i++)
		if (!h->listeners[i].opened) { L = &h->listeners[i]; break; }
	if (!L) {
		fprintf(stderr, "reac-pw: [%s] REAC heard but every listener slot (%d) is in use — "
		        "this segment is NOT served (bounded, reported)\n", name, h->n_slots);
		reac_ifscan_serve_failed(&h->scan, name, monotonic_ns());
		return;
	}
	memset(L, 0, sizeof *L);
	listener_cfg_from_conf(&L->cfg, name, 0);
	if (h->forced_rate != 0) {
		L->cfg.rxcfg.forced_rate = h->forced_rate;
		L->cfg.rate_layer = REAC_CONF_ARGV;
	}
	snprintf(L->cfg.tag, sizeof L->cfg.tag, "[%s] ", name);
	reac_role_swap_init(&L->role_swap, L->cfg.role);
	/* listener_open cleans up after its own refusal (its contract); a feeder
	 * that will not start leaves an opened listener to close, as in main(). */
	int up = listener_open(L, h->loop) == 0;
	if (up && reac_rx_start(&L->rx) != 0) {
		listener_close(L, h->loop);
		up = 0;
	}
	if (!up) {
		memset(L, 0, sizeof *L);
		fprintf(stderr, "reac-pw: [%s] heard, but the segment did not come up — sniffing "
		        "again in %d s\n", name, (int)(REAC_IFSCAN_RETRY_NS / 1000000000ULL));
		reac_ifscan_serve_failed(&h->scan, name, monotonic_ns());
		return;
	}
	L->opened = 1;
	L->rx_started = 1;
	h->served++;
	fprintf(stderr, "reac-pw: [%s] segment up (%s) — %lu served so far\n", name,
	        reac_role_name(L->cfg.role), h->served);
}

static void hearing_drop(struct hearing *h, const char *name, const char *why)
{
	struct listener *L = hearing_listener(h, name);
	if (!L)
		return;
	listener_close(L, h->loop);
	L->opened = 0;
	L->rx_started = 0;
	h->dropped++;
	fprintf(stderr, "reac-pw: [%s] segment dropped — %s\n", name, why);
}

/* Do what the table says. Main loop only. */
static void hearing_apply(struct hearing *h)
{
	struct reac_ifscan_event ev;
	while (reac_ifscan_next(&h->scan, &ev)) {
		const struct reac_ifscan_entry *e = reac_ifscan_find(&h->scan, ev.name);
		switch (ev.verb) {
		case REAC_IFSCAN_LISTEN:
			sniffer_open(h, ev.name);
			break;
		case REAC_IFSCAN_UNLISTEN:
			sniffer_close(h, ev.name);
			fprintf(stderr, "reac-pw: [%s] link down — no longer listening\n", ev.name);
			break;
		case REAC_IFSCAN_SERVE:
			sniffer_close(h, ev.name);
			hearing_serve(h, ev.name);
			break;
		case REAC_IFSCAN_DROP:
			hearing_drop(h, ev.name, e ? "link down past the hold, or the interface went away"
			                           : "the interface went away");
			break;
		case REAC_IFSCAN_KEPT:
			fprintf(stderr, "reac-pw: [%s] link back inside the hold — segment kept "
			        "(flaps so far: %u)\n", ev.name, e ? e->flaps : 0);
			break;
		case REAC_IFSCAN_NONE:
			break;
		}
	}
	if (h->scan.dropped_ev) {
		fprintf(stderr, "reac-pw: hearing: %lu interface events could not be queued\n",
		        h->scan.dropped_ev);
		h->scan.dropped_ev = 0;
	}
	if (h->scan.unbounded) {
		fprintf(stderr, "reac-pw: hearing: %lu Ethernet interfaces beyond the %d tracked — "
		        "NOT watched (bounded, reported)\n", h->scan.unbounded, REAC_IFSCAN_MAX);
		h->scan.unbounded = 0;
	}
}

static void on_hearing_nl_io(void *data, int fd, uint32_t mask)
{
	(void)fd;
	struct hearing *h = data;
	if (mask & SPA_IO_IN)
		reac_ifscan_drain(&h->scan, monotonic_ns());
}

/* The 200 ms poll's share: expire holds, apply whatever the table queued. A
 * listener whose capture socket lost its interface is a DROP here, not a
 * process exit — failure is isolated to its segment (§9). */
static void hearing_poll(struct hearing *h)
{
	if (!h->enabled)
		return;
	uint64_t now = monotonic_ns();
	for (int i = 0; i < h->n_slots; i++) {
		struct listener *L = &h->listeners[i];
		if (L->opened && L->rx_started &&
		    atomic_load_explicit(&L->rx.iface_lost, memory_order_acquire))
			reac_ifscan_gone(&h->scan, L->cfg.rxcfg.source, 0, now);
	}
	reac_ifscan_tick(&h->scan, now);
	hearing_apply(h);
}

static int hearing_start(struct hearing *h, struct pw_loop *loop, struct listener *slots,
                         int n_slots, int forced_rate)
{
	memset(h, 0, sizeof *h);
	h->loop = loop;
	h->listeners = slots;
	h->n_slots = n_slots;
	h->forced_rate = forced_rate;
	uint64_t now = monotonic_ns();
	if (reac_ifscan_open(&h->scan, now) != 0) {
		fprintf(stderr, "reac-pw: cannot watch the interface table over netlink: %s\n",
		        strerror(errno));
		return -1;
	}
	h->nl_io = pw_loop_add_io(loop, reac_ifscan_fd(&h->scan), SPA_IO_IN, false,
	                          on_hearing_nl_io, h);
	if (!h->nl_io) {
		reac_ifscan_close(&h->scan);
		return -1;
	}
	h->enabled = 1;
	int eth = 0;
	for (int i = 0; i < REAC_IFSCAN_MAX; i++)
		if (h->scan.ifs[i].state != REAC_IFSCAN_ABSENT)
			eth++;
	fprintf(stderr, "reac-pw: hearing: %d Ethernet interface(s), %d with link — a segment "
	        "appears where REAC is heard, and drops %d s after link is lost\n",
	        eth, reac_ifscan_count(&h->scan, REAC_IFSCAN_LINKED),
	        (int)(REAC_IFSCAN_DOWN_HOLD_NS / 1000000000ULL));
	hearing_apply(h);
	return 0;
}

static void hearing_stop(struct hearing *h)
{
	if (!h->enabled)
		return;
	for (int i = 0; i < REAC_IFSCAN_MAX; i++)
		if (h->sniff[i].name[0])
			sniffer_close(h, h->sniff[i].name);
	if (h->nl_io)
		pw_loop_destroy_source(h->loop, h->nl_io);
	reac_ifscan_close(&h->scan);
	h->enabled = 0;
}

struct rate_reopen_ctx { struct listener *listeners; int n; struct pw_loop *loop; };

/* Set when a segment's capture socket lost its interface (see reac_rx.h's
 * bound_ifindex). main() turns it into a non-zero exit so the service manager
 * restarts us; read only on the loop thread after pw_main_loop_run returns. */
static int g_iface_lost;

/* ONE main-loop poll (200 ms) for every segment, not per-listener: a
 * listener_close from inside a per-listener timer would free that very timer.
 * Runs on the loop thread, never inside a node callback, so the destroy+rebuild
 * is safe. */
static void on_rate_reopen_timer(void *data, uint64_t exp)
{
	(void)exp;
	struct rate_reopen_ctx *c = data;
	for (int i = 0; i < c->n; i++) {
		struct listener *L = &c->listeners[i];
		if (!L->opened)
			continue;
		/* A LOST INTERFACE IS FATAL — TERMINATE, DO NOT NARRATE.
		 *
		 * The feeder can only detect and latch; ending the process is the main
		 * loop's job, and until it did, detection bought nothing. On 2026-08-29
		 * the S-0808's daemon spotted the vanish, logged it every 2 s for three
		 * minutes, then went quiet when the NIC came back under the same name —
		 * and kept "running" for four more minutes with a dead socket, telling
		 * the operator to bounce a healthy box. A daemon whose socket cannot be
		 * repaired must die so something can restart it: the socket is bound to
		 * an interface that no longer exists, and no amount of waiting rebinds
		 * it. packaging/reac-pw.service carries Restart=on-failure + RestartSec=2,
		 * so a non-zero exit is already a restart; a clean SIGTERM returns 0 and
		 * is left alone.
		 *
		 * Checked before the reopen work below because none of it can succeed
		 * on a segment whose interface is gone. */
		if (L->rx_started &&
		    atomic_load_explicit(&L->rx.iface_lost, memory_order_acquire)) {
			if (g_hear.enabled)
				continue;   /* hearing_poll drops this one segment and keeps the rest */
			g_iface_lost = 1;
			pw_main_loop_quit(g_loop);
			return;
		}
		/* THE SLAVE HALF, and it is polled here for the same reason the master
		 * half is: main's loop thread is the only place a listener may be torn
		 * down and rebuilt. A recorder's door and its answer both live on its
		 * capture node (reac_source_node.h), so this drains that door and
		 * re-publishes the answer — which MOVES while nothing is asserted, as
		 * the hunt ends or a desk drops. */
		if (!L->sink) {
			int back = reac_source_node_take_reopen_role(L->src);
			if (back >= 0) {
				listener_reopen_at_role(L, c->loop, (enum reac_role)back);
				continue;   /* the capture node was rebuilt; nothing more this tick */
			}
			listener_publish_segment(L);
			continue;
		}
		int role = reac_sink_node_take_reopen_role(L->sink);
		if (role >= 0) {
			listener_reopen_at_role(L, c->loop, (enum reac_role)role);
			continue;   /* the master sink is gone after a swap to slave; nothing more this tick */
		}
		int hz = reac_sink_node_take_reopen_rate(L->sink);
		if (hz > 0)
			listener_reopen_at_rate(L, c->loop, hz);
	}
	hearing_poll(&g_hear);
}


int main(int argc, char **argv)
{
	/* HELP IS PURE TEXT AND MUST NOT REQUIRE A CAPABILITY. Asking how to run this
	 * is exactly what an operator does on a machine where the binary has no caps
	 * yet — answering that with the CAP_NET_RAW refusal hides the very sentence
	 * that tells them how to fix it. Answered before the preflight, which then
	 * guards every real start unchanged. */
	/* NO ARGUMENTS IS THE PACKAGED SHAPE, NOT AN ERROR, AND NOTHING NEEDS TO BE
	 * CONFIGURED FOR IT: the daemon hears its segments (see HEARING below). An
	 * operator who wants the text asks for it with --help, which is answered
	 * before the preflight because help is pure text and must never need a
	 * capability. */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		}
	}

	/* Before anything is opened, per §4e: a missing capability must arrive as a
	 * sentence, not as a daemon that runs deaf. */
	capability_preflight();

	/* ---- the CLI template: byte-identical to every invocation before this one.
	 * These are the flags/variables main() always had; they describe ONE
	 * segment (the first --live, or --pcap) and nothing else. Auto-spine's
	 * extra segments never read them — see listener_cfg_from_conf. */
	enum reac_conf_layer rate_layer = REAC_CONF_NONE;
	struct reac_rx_cfg rxcfg = { .kind = REAC_RX_PCAP, .source = NULL, .forced_rate = 0,
	                             .pcap_realtime = 1 };
	const char *tx_if = NULL;
	enum reac_role role = REAC_ROLE_MASTER;   /* default master: preserves current behaviour */
	uint8_t src_mac[6];
	int src_mac_set = 0;
	int box_channels = REAC_SLAVE_BOX_CHANNELS_DEFAULT;  /* slave: our input width */
	const struct reac_box_model *pin_model = NULL;  /* --box: pinned NODE geometry     */
	const char *pin_label = NULL;                   /* --box: pinned node label        */
	const char *box_pin_spec = NULL;      /* --box verbatim, for the wire-wins notice
	                                       * that the wire disagreed with it (once) */
	const char *inst_name = NULL;   /* --name: per-instance node suffix (one master/VLAN) */
	/* --headamp CH:PARAM:VALUE (master role, repeatable): the per-channel head-amp
	 * DMX table the master re-asserts to the box (task #155). At most one cell per
	 * (channel,param); the table set() overwrites a repeat. */
	struct reac_headamp_setting headamps[REAC_HEADAMP_MAX_CH * REAC_HEADAMP_NPARAMS];
	int n_headamps = 0;
	const struct reac_mixer_profile *mixer =
		reac_mixer_profile_by_name("m200");   /* master: which desk generation we speak as */

	/* auto-spine §5: EXTRA `--live` interfaces beyond the template's own (a
	 * repeated flag, or a comma list in one flag). A stable copy is kept here
	 * because the template's own iface below points at argv, which a later
	 * split_list() call must not alias. */
	char first_iface_buf[IFNAMSIZ + 1];
	char extra_ifaces[REAC_PW_MAX_LISTENERS][IFNAMSIZ + 1];
	int n_extra_ifaces = 0;
	int have_live = 0;   /* was --live given at all (vs --pcap, vs neither) */

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--pcap") && i + 1 < argc) {
			rxcfg.kind = REAC_RX_PCAP; rxcfg.source = argv[++i];
		} else if (!strcmp(argv[i], "--live") && i + 1 < argc) {
			char toks[REAC_PW_MAX_LISTENERS][IFNAMSIZ + 1];
			int nt = split_list(argv[++i], toks, REAC_PW_MAX_LISTENERS);
			if (nt == 0) {
				fprintf(stderr, "reac-pw: --live needs at least one interface name\n");
				return 2;
			}
			rxcfg.kind = REAC_RX_LIVE;
			int start = 0;
			if (!have_live) {
				/* The FIRST --live (of possibly several): the template's own
				 * interface, byte-identical to today when it is the only one. */
				snprintf(first_iface_buf, sizeof first_iface_buf, "%s", toks[0]);
				rxcfg.source = first_iface_buf;
				have_live = 1;
				start = 1;
			}
			/* One slot of REAC_PW_MAX_LISTENERS is always the template's own
			 * interface once this branch runs at all, so extras are bounded to
			 * the rest — checked HERE, not left to the later array-assembly
			 * copy to truncate silently (auto-spine: "a segment beyond the
			 * bound is reported, never silently ignored"). */
			for (int k = start; k < nt; k++) {
				if (n_extra_ifaces >= REAC_PW_MAX_LISTENERS - 1) {
					fprintf(stderr, "reac-pw: too many --live interfaces (max %d); "
					        "'%s' is dropped\n", REAC_PW_MAX_LISTENERS, toks[k]);
					continue;
				}
				snprintf(extra_ifaces[n_extra_ifaces++], sizeof extra_ifaces[0],
				        "%s", toks[k]);
			}
		} else if (!strcmp(argv[i], "--rate") && i + 1 < argc) {
			/* Validate before it reaches the ring depth (sample_rate/4): a
			 * negative/garbage rate underflows to a huge depth, next_pow2
			 * overflows to a 0-slot ring with mask 0xFFFFFFFF, and the first
			 * write scribbles the heap. Accept only sane audio rates — the REAC
			 * world is the 44.1k/48k/88.2k/96k families. */
			int rate = atoi(argv[++i]);
			/* ONLY THREE PACES ARE LEGAL: 44.1, 48 and 96 kHz. A Roland desk
			 * offers exactly these and drives the segment at the one chosen;
			 * anything else is not a slower REAC, it is not REAC, and it would
			 * need RE-PACING between the rig clock and the wire — which reac-pw
			 * cannot do, having no TX resampler. This used to accept anything
			 * from 8000 to 192000 and put it on the wire, a cadence no box can
			 * follow, called configuration. */
			if (rate != 44100 && rate != 48000 && rate != 96000) {
				fprintf(stderr, "reac-pw: illegal --rate '%s'. REAC runs at "
				        "44100, 48000 or 96000 Hz and nothing else; anything "
				        "else needs re-pacing, which reac-pw cannot do.\n",
				        argv[i]);
				return 2;
			}
			rxcfg.forced_rate = rate;
			rate_layer = REAC_CONF_ARGV;
		} else if (!strcmp(argv[i], "--tx") && i + 1 < argc) {
			tx_if = argv[++i];
		} else if (!strcmp(argv[i], "--src-mac") && i + 1 < argc) {
			if (parse_mac(argv[++i], src_mac) != 0) {
				fprintf(stderr, "reac-pw: bad --src-mac '%s' (want aa:bb:cc:dd:ee:ff)\n",
				        argv[i]);
				return 2;
			}
			src_mac_set = 1;
		} else if (!strcmp(argv[i], "--role") && i + 1 < argc) {
			if (reac_role_parse(argv[++i], &role) != 0) {
				fprintf(stderr, "reac-pw: unknown --role '%s' (master|slave)\n", argv[i]);
				return 2;
			}
		} else if (!strcmp(argv[i], "--mixer") && i + 1 < argc) {
			/* Master role: which desk GENERATION to speak as (console-model
			 * byte). The grants are box-defined, so a box locks to any profile. */
			mixer = reac_mixer_profile_by_name(argv[++i]);
			if (!mixer) {
				fprintf(stderr, "reac-pw: unknown --mixer '%s'; known:", argv[i]);
				for (int k = 0; reac_mixer_profile_at(k); k++)
					fprintf(stderr, " %s (%s)", reac_mixer_profile_at(k)->name,
					        reac_mixer_profile_at(k)->display);
				fprintf(stderr, "\n");
				return 2;
			}
		} else if (!strcmp(argv[i], "--box-model") && i + 1 < argc) {
			/* Slave role: pick a FIXED-matrix box model (the matrix is law when we
			 * are a stagebox). Selects the config-announce block, the ASCII name
			 * frame, and the width in one choice. */
			const struct reac_box_model *m = reac_box_model_by_token(argv[++i]);
			if (!m) {
				size_t n; const struct reac_box_model *t = reac_box_model_table(&n);
				fprintf(stderr, "reac-pw: unknown --box-model '%s'; known:", argv[i]);
				for (size_t k = 0; k < n; k++)
					fprintf(stderr, " %s (%s)", t[k].token, t[k].display);
				fprintf(stderr, "\n");
				return 2;
			}
			box_channels = m->in_ch;
		} else if (!strcmp(argv[i], "--box-channels") && i + 1 < argc) {
			box_channels = atoi(argv[++i]);
			if (box_channels < 2 || box_channels > REAC_MAX_CHANNELS || (box_channels & 1)) {
				fprintf(stderr, "reac-pw: --box-channels must be even, 2..%d "
				        "(e.g. 8 = S-0808, 16 = S-1608, 32 = S-4000S)\n", REAC_MAX_CHANNELS);
				return 2;
			}
		} else if (!strcmp(argv[i], "--box") && i + 1 < argc) {
			/* MASTER role: the operator's PIN for a FIXED INSTALLATION — the nodes
			 * exist, named and sized, from boot rather than appearing when the box
			 * powers up (operator, 2026-08-22: reac-pw is a daemon in its own right
			 * and a permanent rig pins its patch; openmixer is the autodetect case,
			 * and the two are complementary, not alternatives).
			 *
			 * IT PINS NODE GEOMETRY AND LABEL, AND NOTHING ELSE. This flag was retired
			 * on 2026-08-05 because it also pre-fabricated a head-amp ENROLLMENT from
			 * a guess, and a box granted slots it does not own "links, streams audio,
			 * and silently ignores every head-amp record" (902bf39). That door stays
			 * shut: reac_master_set_box remains fed only by what a box declares about
			 * itself. The pin says what to CALL the ports and how many to make; the
			 * wire says what is actually out there, and where they disagree the WIRE
			 * WINS and says so once (reac_box_pin_notice, autodetect watcher above). */
			const char *spec = argv[++i];
			if (reac_box_pin_parse(spec, &pin_model, &pin_label) != 0) {
				size_t nm; const struct reac_box_model *t = reac_box_model_table(&nm);
				fprintf(stderr, "reac-pw: unknown --box model '%s'; known:", spec);
				for (size_t k = 0; k < nm; k++)
					fprintf(stderr, " %s", t[k].token);
				fprintf(stderr, "  (form: MODEL[:LABEL])\n");
				return 2;
			}
			box_pin_spec = spec;
		} else if (!strcmp(argv[i], "--name") && i + 1 < argc) {
			inst_name = argv[++i];   /* per-instance PW node suffix (multi-master) */
		} else if (!strcmp(argv[i], "--headamp") && i + 1 < argc) {
			/* MASTER role: one per-channel head-amp cell the master re-asserts to
			 * the box (declarative/DMX). Repeatable; the table dedups per cell. */
			if (n_headamps >= (int)(sizeof headamps / sizeof headamps[0])) {
				fprintf(stderr, "reac-pw: too many --headamp settings (max %d)\n",
				        (int)(sizeof headamps / sizeof headamps[0]));
				return 2;
			}
			if (parse_headamp(argv[++i], &headamps[n_headamps]) != 0) {
				fprintf(stderr, "reac-pw: bad --headamp '%s' (want "
				        "CH:phantom|pad|sens:VALUE; CH 0..%d wire channel; "
				        "VALUE 0/1 for phantom|pad, 0..%d raw code for sens)\n",
				        argv[i], REAC_HEADAMP_MAX_CH - 1, REAC_HEADAMP_SENS_MAX);
				return 2;
			}
			n_headamps++;
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	/* Neither --pcap nor --live: the packaged-service shape. Nothing is read from
	 * anywhere to decide which interfaces to serve — the daemon HEARS them. */
	int hearing = rxcfg.source == NULL;
	if (reac_role_validate(role, tx_if != NULL) != 0) {
		fprintf(stderr, "reac-pw: --role slave needs --tx IFNAME (the REAC NIC for the "
		                "upstream return + handshake)\n");
		return 2;
	}
	/* --box declares the box a MASTER serves. As a SLAVE we ARE the box, and our own
	 * width is a different fact with no wire to learn it from: --box-channels /
	 * --box-model. Reject the mix rather than mislead about which one is in force. */
	if (role == REAC_ROLE_SLAVE && pin_model) {
		fprintf(stderr, "reac-pw: --box is a MASTER-role option (it pins the box this "
		                "master serves); a SLAVE's own width is --box-channels or "
		                "--box-model.\n");
		return 2;
	}
	/* --headamp drives the box's preamps — only the MASTER commands them; as a slave
	 * WE are the box and receive them (surfaced in the RX log). Reject the mix. */
	if (role == REAC_ROLE_SLAVE && n_headamps > 0) {
		fprintf(stderr, "reac-pw: --headamp is a master-role option (the master commands "
		                "the box's preamps); a slave receives head-amp records, it does "
		                "not send them\n");
		return 2;
	}

	/* ---- assemble the listener array --------------------------------- */
	struct listener listeners[REAC_PW_MAX_LISTENERS];
	int n_listeners = 0;
	memset(listeners, 0, sizeof listeners);

	if (hearing) {
		/* The slots are filled as segments are heard; see hearing_serve. */
	} else if (rxcfg.source && rxcfg.kind == REAC_RX_PCAP) {
		/* Exactly today: one listener, pcap replay, the CLI template verbatim. */
		n_listeners = 1;
		struct listener_cfg *c = &listeners[0].cfg;
		listener_cfg_defaults(c);
		c->rxcfg = rxcfg;
		c->tx_if = tx_if;
		c->role = role;
		c->mixer = mixer;
		c->inst_name = inst_name;
		memcpy(c->src_mac, src_mac, 6);
		c->src_mac_set = src_mac_set;
		c->box_channels = box_channels;
		c->pin_model = pin_model;
		c->pin_label = pin_label;
		c->box_pin_spec = box_pin_spec;
		memcpy(c->headamps, headamps, sizeof(headamps[0]) * (size_t)n_headamps);
		c->n_headamps = n_headamps;
		c->rate_layer = rate_layer;
	} else {
		/* LIVE: the operator named interface(s) on the command line. */
		const char *ifaces[REAC_PW_MAX_LISTENERS];
		int n_ifaces = 0;
		ifaces[n_ifaces++] = rxcfg.source;
		for (int k = 0; k < n_extra_ifaces && n_ifaces < REAC_PW_MAX_LISTENERS; k++)
			ifaces[n_ifaces++] = extra_ifaces[k];
		n_listeners = n_ifaces;
		for (int i = 0; i < n_ifaces; i++) {
			struct listener_cfg *c = &listeners[i].cfg;
			if (i == 0 && have_live) {
				/* Byte-exact with every invocation before this one. */
				listener_cfg_defaults(c);
				c->rxcfg = rxcfg;
				c->rxcfg.source = ifaces[0];
				c->tx_if = tx_if;
				c->role = role;
				c->mixer = mixer;
				c->inst_name = inst_name;
				memcpy(c->src_mac, src_mac, 6);
				c->src_mac_set = src_mac_set;
				c->box_channels = box_channels;
				c->pin_model = pin_model;
				c->pin_label = pin_label;
				c->box_pin_spec = box_pin_spec;
				memcpy(c->headamps, headamps, sizeof(headamps[0]) * (size_t)n_headamps);
				c->n_headamps = n_headamps;
				c->rate_layer = rate_layer;
			} else {
				listener_cfg_from_conf(c, ifaces[i], i == 0);
				/* A whole-invocation --rate (the ARGV layer) outranks every
				 * per-segment conf file, on EVERY listener — it is the layer
				 * "openmixer uses" (reac_conf.h precedence #1). */
				if (rxcfg.forced_rate != 0) {
					c->rxcfg.forced_rate = rxcfg.forced_rate;
					c->rate_layer = REAC_CONF_ARGV;
				}
			}
		}
	}

	/* Tag every listener for the log, now that N is known. A LONE listener
	 * keeps today's unprefixed lines, byte-identical. */
	for (int i = 0; i < n_listeners; i++) {
		if (n_listeners > 1)
			snprintf(listeners[i].cfg.tag, sizeof listeners[i].cfg.tag, "[%s] ",
			        listeners[i].cfg.rxcfg.source ? listeners[i].cfg.rxcfg.source : "?");
		else
			listeners[i].cfg.tag[0] = '\0';
		/* SEED THE ROLE RECORD FROM THE ROLE THIS SEGMENT BOOTS IN, before any
		 * engine exists. Seeded here and not inside listener_open, which runs
		 * again on every re-open and would overwrite the console's assertion
		 * with the role it happens to be re-opening as. */
		reac_role_swap_init(&listeners[i].role_swap, listeners[i].cfg.role);
	}

	pw_init(&argc, &argv);

	g_loop = pw_main_loop_new(NULL);
	struct pw_loop *loop = pw_main_loop_get_loop(g_loop);
	pw_loop_add_signal(loop, SIGINT, on_signal, NULL);
	pw_loop_add_signal(loop, SIGTERM, on_signal, NULL);

	/* ONE shared loop, N listeners opened against it (auto-spine §5). A lone
	 * listener that fails to open ends the process exactly as it always has;
	 * with more than one, a segment's refusal is that segment's problem, not
	 * every other segment's — the daemon keeps whatever else came up. */
	if (hearing) {
		if (hearing_start(&g_hear, loop, listeners, REAC_PW_MAX_LISTENERS,
		                  rxcfg.forced_rate) != 0) {
			pw_main_loop_destroy(g_loop);
			pw_deinit();
			return 1;
		}
		n_listeners = REAC_PW_MAX_LISTENERS;
	}

	int n_opened = 0;
	for (int i = 0; i < n_listeners && !hearing; i++) {
		if (listener_open(&listeners[i], loop) != 0) {
			if (n_listeners == 1) {
				pw_main_loop_destroy(g_loop);
				pw_deinit();
				return 1;
			}
			fprintf(stderr, "reac-pw: %sthis segment did not come up; the other "
			        "listener(s) are unaffected (auto-spine §5: one daemon, "
			        "independent segments)\n", listeners[i].cfg.tag);
			continue;
		}
		listeners[i].opened = 1;
		n_opened++;
	}
	if (n_opened == 0 && !hearing) {
		fprintf(stderr, "reac-pw: no segment came up; nothing to run\n");
		pw_main_loop_destroy(g_loop);
		pw_deinit();
		return 1;
	}

	int any_running = 0;
	for (int i = 0; i < n_listeners && !hearing; i++) {
		if (!listeners[i].opened)
			continue;
		if (reac_rx_start(&listeners[i].rx) != 0) {
			fprintf(stderr, "reac-pw: %sfailed to start RX feeder\n", listeners[i].cfg.tag);
			if (n_listeners == 1) {
				listener_close(&listeners[0], loop);
				pw_main_loop_destroy(g_loop);
				pw_deinit();
				return 1;
			}
			listener_close(&listeners[i], loop);
			listeners[i].opened = 0;
			continue;
		}
		listeners[i].rx_started = 1;
		any_running = 1;
	}
	if (!any_running && !hearing) {
		fprintf(stderr, "reac-pw: no segment is running; nothing to do\n");
		pw_main_loop_destroy(g_loop);
		pw_deinit();
		return 1;
	}

	struct rate_reopen_ctx rrctx = { listeners, n_listeners, loop };
	struct spa_source *rate_timer = pw_loop_add_timer(loop, on_rate_reopen_timer, &rrctx);
	if (rate_timer) {
		struct timespec first = { 0, 200 * 1000000L };
		struct timespec interval = { 0, 200 * 1000000L };
		pw_loop_update_timer(loop, rate_timer, &first, &interval, false);
	}

	pw_main_loop_run(g_loop);

	hearing_stop(&g_hear);
	for (int i = 0; i < n_listeners; i++)
		if (listeners[i].opened)
			listener_close(&listeners[i], loop);

	pw_main_loop_destroy(g_loop);
	pw_deinit();
	/* Non-zero so a Restart=always unit brings us back on the live interface;
	 * a clean SIGTERM shutdown still returns 0. */
	return g_iface_lost ? 1 : 0;
}
