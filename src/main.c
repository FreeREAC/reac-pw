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
 */

#include "reac_ring.h"
#include "reac_rx.h"
#include "reac_source_node.h"
#include "reac_sink_node.h"
#include "reac_slave.h"
#include "reac_role.h"
#include "reac_mac.h"
#include "reac_ctrl.h"        /* enum reac_headamp_param, REAC_HEADAMP_SENS_MAX */
#include "reac_headamp_tx.h"  /* struct reac_headamp_setting */
#include "reac_box_pin.h"     /* --box MODEL[:LABEL]: the fixed-installation pin */
#include "reac_conf.h"     /* the LAYERED config lookup + which layer answered */
#include "reac_seglock.h"    /* one master per segment, across processes */

#include <pipewire/pipewire.h>
#include <reac/reac.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <linux/if_packet.h>

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

/* Parse "CH:PARAM:VALUE" (a master-role --headamp arg) into *out. CH is the WIRE
 * channel (0..REAC_HEADAMP_MAX_CH-1); PARAM is phantom|pad|sens; VALUE is 0/1 for
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

static void usage(const char *p)
{
	fprintf(stderr,
	  "usage: %s (--pcap FILE | --live IFNAME) [--role master|slave] [--rate R] [--tx IFNAME]\n"
	  "         [--mixer M] [--box MODEL[:LABEL]] [--box-channels N] [--name NAME] [--src-mac M]\n"
	  "  --pcap FILE   replay a REAC capture (offline test, reuses pcap_source)\n"
	  "  --live IFNAME live AF_PACKET 0x8819 capture (reuses reac_capture; needs CAP_NET_RAW)\n"
	  "  --role R      master (default; WE drive the handshake + own the clock — a box\n"
	  "                slaves to us) | slave (an external master drives; we lock to its\n"
	  "                cadence + return our inputs upstream)\n"
	  "  --rate R      the REAC sample rate: 44100, 48000 or 96000.\n"
	  "                Default 96000 in the MASTER role (a master DEFINES the rate;\n"
	  "                there is nothing to detect on a segment nobody is driving).\n"
	  "                As a SLAVE, auto-detected from the wire cadence.\n"
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
	  "                CH = wire channel 0..39; PARAM = phantom|pad|sens; VALUE = 0/1\n"
	  "                for phantom|pad, 0..55 raw SENS code for sens. RIG-GATED.\n"
	  "  --src-mac M   our on-wire source MAC (aa:bb:cc:dd:ee:ff). Default for BOTH\n"
	  "                roles: the --tx NIC's OWN hardware address, verbatim — our frames\n"
	  "                carry OUR identity (real boxes and desks sync to it; a borrowed\n"
	  "                MAC collides with the real device and makes captures ambiguous).\n"
	  "environment (see docs/ENV-KNOBS.md; unset = default behavior, byte-identical):\n"
	  "  REACPW_GRANT_DWELL_S=N  master role: hold the recognized-but-ungranted dwell\n"
	  "                for N whole seconds before the grant burst (default: the built-in\n"
	  "                ~1.6 s dwell; a real M-200 holds a cold box ~27 s)\n"
	  "  REAC_DEBUG=1  opt-in RX/source telemetry on stderr (~every 2 s: frame/dup/gap\n"
	  "                counters, ring fill, active channels)\n"
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
	  "  REACPW_RATE_MATCH=0  master role: publish NO io_rate_match on the sink,\n"
	  "                so the graph/wire difference has nowhere to go but the\n"
	  "                depth guard's discard. For A/B measurement only.\n", p);
}

/* MASTER autodetect — the only mode there is. A main-loop watcher that polls the box
 * the pacer recognized on the wire and (re)sizes the reac-capture / reac-playback
 * nodes to its real widths. Starting with NO BOX PRESENT is the normal state: nothing
 * plugged, nothing in the graph, and the nodes appear sized to the box the moment it
 * declares itself. Node create/destroy MUST run on the main/loop thread; recognition runs
 * in the RT pacer thread, which hands the model over via the pacer's atomic
 * recognized_box (read here through reac_sink_node_recognized_box) — so no pw_* call is
 * ever made from the pacer thread. The library owns the node lifecycle: this reads the
 * width + label and calls the two ensure() entry points, nothing more. */
struct autodetect_ctx {
	struct reac_source_node    **src;   /* main's source slot (created/rebuilt here) */
	struct reac_sink_node       *sink;  /* the master engine (owns the recognizer)   */
	struct reac_source_node_cfg  scfg;  /* stable reac-capture create args           */
	const struct reac_box_model *last;  /* last model acted on (edge-detects changes) */
	const char                  *pin;   /* a retired --box value, for the disagreement
	                                     * notice; NULL once reported (report ONCE)  */
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
			fprintf(stderr, "reac-pw: --box pinned '%.*s', the wire says %s — the "
			        "WIRE WINS and the nodes are now sized to it. The pin is still what "
			        "this segment shows before a box is powered, so fix it if this box "
			        "is the permanent one.\n",
			        (int)strcspn(pin, ":"), pin, bm->display);
	}
	/* Everything derived from the recognized in_ch/out_ch — no per-model branches. */
	if (reac_source_node_ensure(c->src, &c->scfg, bm->in_ch, bm->display) != 0)
		fprintf(stderr, "reac-pw: could not size reac-capture to %d ch (%s)\n",
		        bm->in_ch, bm->display);
	if (reac_sink_node_ensure(c->sink, bm->out_ch, bm->display) != 0)
		fprintf(stderr, "reac-pw: could not size reac-playback to %d ch (%s)\n",
		        bm->out_ch, bm->display);
	fprintf(stderr, "reac-pw: autodetected %s -> reac-capture %d in / reac-playback "
	        "%d out\n", bm->display, bm->in_ch, bm->out_ch);
}

int main(int argc, char **argv)
{
	/* Before anything is opened, per §4e: a missing capability must arrive as a
	 * sentence, not as a daemon that runs deaf. */
	capability_preflight();

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

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--pcap") && i + 1 < argc) {
			rxcfg.kind = REAC_RX_PCAP; rxcfg.source = argv[++i];
		} else if (!strcmp(argv[i], "--live") && i + 1 < argc) {
			rxcfg.kind = REAC_RX_LIVE; rxcfg.source = argv[++i];
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
	if (!rxcfg.source) {
		usage(argv[0]);
		return 2;
	}
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

	/* SAMPLE RATE — the master chooses it; the box follows.
	 *
	 * On a real Roland desk the operator selects the REAC rate from a menu. The
	 * desk drives the segment at that rate and every stagebox locks to it — a box
	 * has no rate setting of its own. reac-pw is the master here, so `--rate` is
	 * the same choice, and it is honoured as given.
	 *
	 * A previous revision of this comment claimed the box infers its rate from the
	 * DESK IDENTITY (console byte 01 = OHRCA => 96 kHz, 00 = V-Mixer => 48 kHz
	 * only) and clamped --rate to match. That was an inference, never
	 * demonstrated, and it is wrong: the identity byte says which desk we
	 * impersonate, not which rate the operator picked.
	 *
	 * Beware this paragraph's history — the same block also asserted the upstream
	 * "+2 bytes" were the Ethernet FCS, falsified 2026-07-25 (a real OHRCA CRC-16;
	 * docs/OHRCA-UPSTREAM-DUPLICATE-FRAMES.md, fixtures UP32A/UP32B). Two wrong
	 * claims from one comment: state what is measured, mark the rest open (#73).
	 *
	 * Cadence is fps = rate/12 at every rate — 12 samples per frame is invariant,
	 * so a higher rate sends the same frames more often, nothing else changes. */

	/* THE MASTER'S RATE DEFAULT, and the provenance of whatever it ends up being.
	 *
	 * A MASTER DEFINES THE RATE; THERE IS NOTHING TO DETECT. Auto-detect is a
	 * slave's default and it is the right one there — a slave joins a segment
	 * somebody else is already driving, so reading the cadence off the wire is the
	 * only honest thing it can do. A master drives a segment that is SILENT until
	 * it speaks, so "auto" does not resolve to the operator's intent, it resolves
	 * to whatever the fallback happens to be, and nothing on screen says which.
	 *
	 * The default is 96 kHz by the operator's ruling (2026-08-23): "96k is 96kHz
	 * and should be the default reac clock rate." A Roland desk offers 44.1/48/96
	 * and drives the segment at the one chosen; this is that menu's default
	 * position, not a detection result.
	 *
	 * AND THE RATE IS PRINTED WITH WHERE IT CAME FROM. Three sources have
	 * disagreed on this rig at once — a command line, an environment file nothing
	 * read, and this default — and the disagreement was invisible because the
	 * startup line said only the number. A mechanical gate beats a rule anyone has
	 * to remember: the journal now carries the provenance beside the value, so a
	 * 96 k master pointed at a 48 k segment says so in its first two lines. */
	if (role == REAC_ROLE_MASTER && rxcfg.forced_rate == 0) {
		/* Walk the LAYERS before falling back to the compiled-in default. The
		 * order is declared in reac_conf.h and pinned by test_reac_conf; this
		 * call is the only place it is implemented, so there is one order and
		 * not one per reader. */
		char v[64];
		const char *seg = (rxcfg.kind == REAC_RX_LIVE) ? rxcfg.source : NULL;
		enum reac_conf_layer got = reac_conf_lookup("REAC_RATE", seg, NULL,
		                                           v, sizeof v);
		if (got != REAC_CONF_NONE) {
			int r = atoi(v);
			if (r == 44100 || r == 48000 || r == 96000) {
				rxcfg.forced_rate = r;
				rate_layer = got;
			} else {
				/* A layer that answered with nonsense must SAY so and be
				 * skipped, not silently drop us to the built-in with no
				 * explanation — that is how a config file gets blamed for
				 * working and a default gets blamed for not. */
				fprintf(stderr, "reac-pw: ignoring REAC_RATE='%s' from %s — REAC "
				        "runs at 44100, 48000 or 96000 Hz and nothing else\n",
				        v, reac_conf_layer_name(got));
			}
		}
		if (rxcfg.forced_rate == 0) {
			rxcfg.forced_rate = REAC_MASTER_DEFAULT_RATE;
			rate_layer = REAC_CONF_BUILTIN;
		}
	}
	/* NAME THE LAYER THAT ANSWERED, not merely the value. A layered config that
	 * cannot tell you which layer won is a debugging trap, and this rig has
	 * already spent a morning on exactly that class of confusion: three sources
	 * disagreed about the rate at once and the startup line printed only the
	 * number. */
	fprintf(stderr, "reac-pw: REAC rate = %d Hz (%d pps), from %s\n",
	        rxcfg.forced_rate, rxcfg.forced_rate / REAC_SAMPLES_PER_PKT,
	        rate_layer == REAC_CONF_NONE ? "auto-detect from the wire cadence"
	                                     : reac_conf_layer_name(rate_layer));

	/* The role picks which stream RX decodes (see DESIGN's role table): as
	 * MASTER our capture is a box's upstream return (its input channels,
	 * box-width braided frames); as SLAVE it is the master's 40-ch downstream
	 * broadcast. The wire carries both; the gate keeps them apart. */
	rxcfg.accept = (role == REAC_ROLE_MASTER) ? REAC_RX_ACCEPT_UPSTREAM
	                                          : REAC_RX_ACCEPT_DOWNSTREAM;

	pw_init(&argc, &argv);

	struct reac_ring ring;
	struct reac_rx rx;
	if (reac_rx_open(&rx, &rxcfg, &ring) != 0) {
		fprintf(stderr, "reac-pw: cannot open source '%s'\n", rxcfg.source);
		return 1;
	}
	fprintf(stderr, "reac-pw: recovered REAC rate = %d Hz (%d pps), rx stream = %s\n",
	        rx.sample_rate, rx.sample_rate / REAC_SAMPLES_PER_PKT,
	        rxcfg.accept == REAC_RX_ACCEPT_UPSTREAM
	          ? "box upstream return (box-width)" : "master downstream (40 ch)");

	g_loop = pw_main_loop_new(NULL);
	struct pw_loop *loop = pw_main_loop_get_loop(g_loop);
	pw_loop_add_signal(loop, SIGINT, on_signal, NULL);
	pw_loop_add_signal(loop, SIGTERM, on_signal, NULL);

	/* The reac-capture source is created AFTER the TX side, because whether to DEFER
	 * it depends on whether a recognizer (the master pacer) exists. In pure autodetect
	 * (master + a live TX pacer) it is deferred: nothing plugged -> nothing in the
	 * graph, and the node appears sized to the box the moment it is recognized (the
	 * autodetect timer below). Every other mode (slave, or pcap / no-TX master) has no
	 * recognizer, so the node is created at its startup width. The cfg bundles the
	 * process-lifetime constants so a later resize needs only the width + label. */
	struct reac_source_node *src = NULL;
	struct reac_source_node_cfg src_cfg = {
		.loop = loop, .ring = &ring, .rx = &rx, .sample_rate = rx.sample_rate,
		.inst = inst_name, .master_role = (role == REAC_ROLE_MASTER),
	};

	/* TX side: who drives the handshake + the clock depends on the role.
	 *   master -> reac:playback sink: WE encode the graph downstream + the pacer
	 *             drives the cdea/cfea grant + owns the clock (a box slaves to us).
	 *   slave  -> reac_slave engine: an external master drives; we lock to its
	 *             cadence + return our input channels upstream at the box's slots. */
	struct reac_sink_node *sink = NULL;
	struct reac_slave slave;
	int slave_open = 0;
	struct reac_ring tx_ring;
	int tx_ring_init = 0;

	if (tx_if && role == REAC_ROLE_MASTER) {
		/* Default master MAC = THIS NIC's own address (reac_mac.h); --src-mac
		 * overrides it. The mixer profile sets only the console-model byte. */
		uint8_t master_mac_buf[6];
		if (!src_mac_set && reac_mac_default_src(tx_if, master_mac_buf) != 0)
			fprintf(stderr, "reac-pw: could not read %s hardware address for the "
			        "master source MAC; using the locally-administered fallback\n",
			        tx_if);
		const uint8_t *master_src = src_mac_set ? src_mac : master_mac_buf;
		reac_ring_init(&tx_ring, REAC_MAX_CHANNELS, (uint32_t)(rx.sample_rate / 4));
		tx_ring_init = 1;
		struct reac_sink_cfg scfg = { .ifname = tx_if,
		                              /* The graph filter is DEFERRED until a box is
		                               * recognized (reac_sink_node_new leaves it at 0
		                               * and the autodetect watcher sizes it), so this
		                               * is only the ceiling. */
		                              .channels = REAC_MAX_CHANNELS,
		                              .sample_rate = rx.sample_rate,
		                              .src_mac = master_src, .master_mac = NULL,
		                              /* THE FAMILY, as configured. It does NOT come from the
                               * rate: the two are independent settings and each is
                               * obeyed as given (see reac_master.h). */
                              .console_field = mixer->console_field,
		                              .inst = inst_name, .label = NULL,
		                              .headamps = n_headamps ? headamps : NULL,
		                              .n_headamps = n_headamps,
		                              /* #75: default OFF -> the pacer free-runs on
		                               * CLOCK_MONOTONIC exactly as it always has. */
		                              .clock_follow = getenv("REACPW_CLOCK_FOLLOW") != NULL,
		                              /* #77: unset -> nothing is designated and the
		                               * name heuristic alone grades the reference. */
		                              .clock_ref = getenv("REACPW_CLOCK_REF"),
		                              /* Slot-debt budget. Unset -> the measured
		                               * default; see reac_pacer.h. */
		                              .catchup_max_slots = getenv("REACPW_CATCHUP_MAX_SLOTS")
		                                  ? atoi(getenv("REACPW_CATCHUP_MAX_SLOTS"))
		                                  : 0,
		                              /* Rate matching is ON unless explicitly
		                               * disabled; the off case exists to measure
		                               * the two levers apart, not to be run. */
		                              .rate_match_off =
		                                  (getenv("REACPW_RATE_MATCH") &&
		                                   atoi(getenv("REACPW_RATE_MATCH")) == 0)
		                                  ? -1 : 0 };
		/* CLAIM THE SEGMENT BEFORE THE FIRST FRAME. Driving is what takes the
		 * lock; RX above has been running unlocked, which is correct — observing a
		 * segment is a copy and must stay safe beside somebody else's master. */
		static struct reac_seglock seglock;
		int claimed = reac_seglock_claim(&seglock, tx_if);
		if (claimed == -1) {
			fprintf(stderr,
			    "reac-pw: REFUSING to master '%s' — another process already holds\n"
			    "         that segment (%s). Two masters on one segment is the\n"
			    "         fault this lock exists to make impossible; it has cost an\n"
			    "         evening once and corrupted a live measurement once.\n"
			    "         Nothing is taken over automatically: stop the holder, or\n"
			    "         drive a different segment. Who holds it:\n"
			    "           grep %s /proc/net/unix\n",
			    tx_if, seglock.name, seglock.name);
			exit(1);
		}
		if (claimed == -2)
			fprintf(stderr, "reac-pw: could not claim a segment lock for '%s' "
			        "(interface or netns unreadable); proceeding UNPROTECTED\n",
			        tx_if);

		sink = reac_sink_node_new(loop, &tx_ring, &scfg); /* encodes + emits REAC */
		if (!sink) {
			/* A master with no TX is not a degraded master, it is a silent one:
			 * it probes nothing, grants nothing and syncs no box, while every
			 * other sign of health stays green. Refuse instead. */
			explain_tx_failure(tx_if);
			exit(1);   /* a refusal, before the loop ever runs */
		} else {
			fprintf(stderr, "reac-pw: MASTER role (%s profile) on '%s' — "
			        "event-driven establishment: probing until the box's "
			        "cold-connect (cdea 04 03) arrives; FSM/RX transcript on "
			        "stderr\n", mixer->display, tx_if);
			if (n_headamps)
				fprintf(stderr, "reac-pw: head-amp DMX send armed — %d cell(s), "
				        "re-asserted once established (RIG-GATED: verify 48V at the "
				        "XLR pins)\n", n_headamps);
		}
	} else if (tx_if && role == REAC_ROLE_SLAVE) {
		/* The slave returns its OWN input channels (a box width) upstream. The PCM
		 * for them would come from a reac:return sink; for now the ring is the
		 * carrier and the slave emits silent/own-input FILLER until that sink is
		 * linked. The engine learns the master MAC from the wire — never set here. */
		uint8_t box_mac[6];
		if (src_mac_set) {
			memcpy(box_mac, src_mac, 6);
		} else if (reac_mac_default_src(tx_if, box_mac) != 0) {
			/* NIC hwaddr unreadable — the locally-administered fallback is still
			 * on-wire safe (no manufacturer carries it), but note it so an
			 * ambiguous capture is explained. */
			fprintf(stderr, "reac-pw: could not read %s hardware address for the box "
			        "source MAC; using the locally-administered fallback\n", tx_if);
		}
		const uint8_t *slave_src = box_mac;
		fprintf(stderr, "reac-pw: slave box source MAC = "
		        "%02x:%02x:%02x:%02x:%02x:%02x%s\n",
		        box_mac[0], box_mac[1], box_mac[2], box_mac[3], box_mac[4], box_mac[5],
		        src_mac_set ? " (--src-mac override)"
		                    : " (this NIC's own address; --src-mac overrides)");
		reac_ring_init(&tx_ring, REAC_MAX_CHANNELS, (uint32_t)(rx.sample_rate / 4));
		tx_ring_init = 1;
		struct reac_slave_cfg slcfg = { .ifname = tx_if,
		                                .box_channels = box_channels,
		                                .sample_rate = rx.sample_rate,
		                                .src_mac = slave_src };
		if (reac_slave_open(&slave, &slcfg, &tx_ring) == 0) {
			slave_open = 1;
			if (reac_slave_start(&slave) == 0) {
				reac_slave_set_phy_up(&slave, 1);  /* PHY up: begin the establishment */
				fprintf(stderr, "reac-pw: SLAVE role on '%s' (%d-ch upstream return) — "
				        "responding to an external master, locked to its cadence\n",
				        tx_if, box_channels);
			} else {
				fprintf(stderr, "reac-pw: slave engine thread failed to start\n");
				reac_slave_close(&slave); slave_open = 0;
			}
		} else {
			fprintf(stderr, "reac-pw: slave engine not created (AF_PACKET on '%s' "
			        "failed — need CAP_NET_RAW?)\n", tx_if);
		}
	}

	/* Now that we know whether a recognizer exists (master + a live TX pacer), either
	 * DEFER the box nodes to autodetect or expose the source at its startup width. */
	struct autodetect_ctx adc = {0};
	struct spa_source *ad_timer = NULL;
	/* #75: the RX feeder is the BOX clock reference's measurement source — it already
	 * tracks the box's counter slope and publishes a filtered ppm error. The sink's
	 * existing 200 ms timer forwards it to the pacer's discipline. Wired
	 * unconditionally; it is only ever read when clock following is enabled. */
	if (sink)
		reac_sink_node_set_rate_source(sink, &rx);
	if (role == REAC_ROLE_MASTER && sink) {
		/* Pure autodetect: the pacer recognizes the box on the wire; a 200 ms main-
		 * loop watcher then (re)sizes reac-capture / reac-playback to its widths. No
		 * box node exists until then (nothing plugged = nothing in the graph). */
		adc.src = &src;
		adc.sink = sink;
		adc.scfg = src_cfg;
		adc.pin  = box_pin_spec;      /* reported once, if the wire disagrees */
		/* #208: let the sink's badge timer keep the reac-capture node's link-state /
		 * box-model / box-width in sync (it has no pacer handle of its own). Same source
		 * slot the autodetect watcher rebuilds, so a live box-width change is followed. */
		reac_sink_node_set_peer_source(sink, &src);
		ad_timer = pw_loop_add_timer(loop, on_autodetect_timer, &adc);
		if (ad_timer) {
			struct timespec first = { 0, 200 * 1000000L };
			struct timespec interval = { 0, 200 * 1000000L };
			pw_loop_update_timer(loop, ad_timer, &first, &interval, false);
		}
		if (pin_model) {
			/* The fixed-install pin: put the nodes on the graph NOW, at the pinned
			 * width and name, so the patch exists before the box is powered. This
			 * is the same pair of calls the autodetect watcher makes on
			 * recognition, so a box that later declares something else simply
			 * re-sizes them — no separate "pinned" code path to diverge. */
			if (reac_source_node_ensure(&src, &src_cfg, pin_model->in_ch, pin_label) != 0 ||
			    reac_sink_node_ensure(sink, pin_model->out_ch, pin_label) != 0) {
				fprintf(stderr, "reac-pw: --box: could not size the nodes to %s\n",
				        pin_model->display);
				return 1;
			}
			fprintf(stderr, "reac-pw: MASTER pinned --box %s — reac-capture %d ch / "
			        "reac-playback %d ch labelled '%s', present from boot. The pin names "
			        "and sizes the ports; the WIRE still decides what is enrolled, and "
			        "outranks the pin if a different box declares itself.\n",
			        pin_model->token, pin_model->in_ch, pin_model->out_ch, pin_label);
		} else {
			fprintf(stderr, "reac-pw: MASTER autodetect — reac-capture / reac-playback "
			        "appear sized to the box once it is recognized on the wire\n");
		}
	} else {
		/* No recognizer (slave, or pcap / no-TX master): expose the source now, at
		 * the full 40-slot fabric. With no recognizer there is nothing that could
		 * honestly narrow it to a box, and nothing may pretend otherwise. */
		if (reac_source_node_ensure(&src, &src_cfg, 0, NULL) != 0) {
			fprintf(stderr, "reac-pw: failed to create reac:capture node\n");
			return 1;
		}
	}

	if (reac_rx_start(&rx) != 0) {
		fprintf(stderr, "reac-pw: failed to start RX feeder\n");
		return 1;
	}

	pw_main_loop_run(g_loop);

	reac_rx_stop(&rx);
	if (ad_timer)
		pw_loop_destroy_source(loop, ad_timer);   /* stop the autodetect watcher first */
	reac_source_node_destroy(src);                /* may be NULL (never recognized) */
	reac_sink_node_destroy(sink);
	if (slave_open) {
		reac_slave_stop(&slave);
		reac_slave_close(&slave);
	}
	if (tx_ring_init)
		reac_ring_free(&tx_ring);
	reac_rx_close(&rx);
	reac_ring_free(&ring);
	pw_main_loop_destroy(g_loop);
	pw_deinit();
	return 0;
}
