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
 * AND THE ROLE COMES OUT OF THE SAME HEARING (reac_hunt.h): no master on the wire
 * and a box present -> we drive, probe, grant; a DESK mastering it -> we join as a
 * slave and follow its pace; a STAGEBOX mastering it -> refused with the remedy named,
 * never fought. `REAC_ROLE_<segment>` overrides that outright; a bare `REAC_ROLE` is
 * only the floor for a segment nobody has heard yet, and it is superseded out loud.
 *
 * Only the FIRST --live segment honours the per-box flags below (--tx/--role/
 * --mixer/--name/--headamp/--box/--src-mac/--box-channels/--box-model),
 * exactly as every invocation before this one; every other segment — and
 * every HEARD one — reads its own REAC_TX/REAC_ROLE/REAC_MIXER/REAC_NAME/
 * REAC_HEADAMP/REAC_BOX_CHANNELS from the layered conf, keyed by its own name
 * (reac_conf.h: `REAC_ROLE_<segment>` above `REAC_ROLE`). This is what lets a
 * single-interface invocation stay BYTE-IDENTICAL to today: a lone listener
 * is the same code path it always was, just reached through an array of one. */

#include <reac/transport/reac_ring.h>
#include <reac/transport/reac_rx.h>
#include "reac_source_node.h"
#include "reac_sink_node.h"
#include <reac/transport/reac_slave.h>
#include <reac/transport/reac_tap.h>       /* the PASSIVE role: serve what is heard, send nothing */
#include "reac_role_cfg.h"   /* the reac.cfg.role vocabulary + refusal codes */
#include <reac/transport/reac_role_swap.h>  /* the role swap's lifecycle answer (arbitration §8) */
#include <reac/transport/reac_segment_ident.h>  /* the segment identity + a slave's own answer set */
#include "reac_watch.h"          /* what a fresh verdict means on a segment already up */
#include <reac/reac_role.h>
#include "reac_rate_cfg.h"
#include <reac/transport/reac_mac.h>
#include <reac/reac_ctrl.h>        /* enum reac_headamp_param, REAC_HEADAMP_SENS_MAX */
#include <reac/reac_link_state.h>  /* reac_box_master_model — the width-to-model row, 0.5.2 */
#include <reac/reac_headamp_tx.h>  /* struct reac_headamp_setting */
#include "reac_box_pin.h"     /* --box MODEL[:LABEL]: the fixed-installation pin */
#include <reac/transport/reac_conf.h>     /* the LAYERED config lookup + which layer answered */
#include <reac/reac_envflag.h>  /* one reading of a boolean knob, for every boolean knob */
#include <reac/transport/reac_seglock.h>    /* one master per segment, across processes */
#include <reac/transport/reac_ifscan.h>     /* which interfaces to sniff, which are segments */
#include "reac_declared_vlan.h"   /* the VLAN segments the operator DECLARED, minted at start */
#include "reac_segconf.h"         /* the ONE override file: per-segment role and ignore */
#include "reac_roster.h"          /* every segment as one node's props (amendment §B) */
#include "reac_roster_node.h"     /* ...and the port-less node that carries them */
#include "reac_link_budget.h"     /* what a master costs its physical port, and whether it fits */
#include "reac_code.h"            /* the stable token vocabulary for refusals + status lines */
#include "reac_knobs.h"           /* every env/conf knob, discovered AND PUBLISHED */
#include <reac/reac_tunables.h>   /* the daemon SETS what libreac used to read via getenv */
#include <reac/transport/reac_topo.h>       /* is this NIC a trunk, and which VLANs carry REAC */
#include <reac/transport/reac_vlan.h>       /* the <parent>.<vid> netdevs the answer needs */
#include <reac/reac_disco.h>      /* the sniffer's bar: a frame that IS REAC gear */
#include <reac/reac_hunt.h>       /* which end of the pairing a heard segment takes */
#include <reac/reac_knock.h>      /* waking a cold box on a wire nobody pinned */
#include <reac/reac_tapwait.h>    /* how long an unplaced sighting holds the hunt off a wire */
#include "reac_wake.h"       /* waking a box that DROPPED, which no frame can do */
#include <reac/transport/reac_carrier.h>    /* is there a cable in this interface */
#include "reac_node_recover.h" /* what to do about a node we built that is not there */

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
#include <sys/stat.h>
#include <dirent.h>
#include <pwd.h>
#include <linux/if_packet.h>
#include <net/if.h>           /* IFNAMSIZ */

/* THE ONE OVERRIDE (docs/design/specs/2026-09-16-segments-and-roles-are-autodetected.md).
 * Read once at start, before any socket, and asked about every segment thereafter. Nothing
 * else may pin a role: `REAC_ROLE` and `REAC_ROLE_<segment>` are retired in every layer,
 * because the file that carried them is GENERATED and a generated file goes stale silently
 * — on 2026-09-16 it pinned three VLAN segments `tap` a rig ago and the desk moved no
 * audio with every node up. A LIVE role change is `reac.cfg.role` on the segment's door,
 * not a re-read of this. */
static struct reac_segconf g_segconf;

/* THE DAEMON'S OWN ROW ON THE GRAPH (spec amendment 2026-09-16 third, §B). One node, no
 * ports, every segment in its props — derived from the tables below on a 500 ms tick and
 * published as a DELTA, so a state change moves properties and never a node id. */
static struct reac_roster g_roster;
static struct reac_roster_node *g_roster_node;
static int g_roster_said;   /* the "on the graph" line waits for the node's real id */

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

/* ---- root refusal (2026-09-18) ---------------------------------------------
 *
 * A ROOT INSTANCE IS NEVER THE CONSOLE'S. This daemon needs no root: file
 * capabilities on the binary (cap_net_raw, cap_net_admin, cap_sys_nice — the
 * capability preflight below reads the very same effective set) already grant a
 * console-user process everything it opens. A root run would ALSO be able to
 * open raw sockets and mint VLAN sub-interfaces that the console user then
 * cannot close, and — the defect that forced this check — it runs under a
 * DIFFERENT systemd --user manager (root's, not the console session's, spawned
 * the moment `sudo` activity starts one), so it binds the SAME abstract
 * segment-lock socket as the console's own daemon (abstract AF_UNIX sockets are
 * scoped by NETWORK namespace, and root's user manager shares the host's).
 * Measured 2026-09-18: `sudo dnf install reac-pw-1.0.18` globally enabled the
 * packaged unit (packaging fix, same release), and the next root user manager
 * started a second reac-pw that won the lock and left the console daemon
 * logging "the segment is held" — every box vanished until the root instance
 * was killed by hand.
 *
 * EFFECTIVE, NOT REAL, UID. This binary carries no setuid bit — privilege comes
 * only from file capabilities (capability_preflight() below reads CapEff, the
 * same effective set) — so real and effective uid agree on every path this
 * daemon is actually started from. geteuid() is what is asked because it is the
 * identity the kernel will charge for every socket this process opens next.
 * Checked before capability_preflight() on purpose: uid 0 carries every
 * capability already, so the capability check alone would pass silently and
 * hide exactly this failure.
 *
 * NOT EVERY uid-0 IS THE HOST'S ROOT. Every capability-gated whole-binary test
 * under tests/ (25 of them) runs this same binary through
 * `unshare -r --map-root-user`, which maps ONE unprivileged host uid to uid 0
 * INSIDE A NEW USER NAMESPACE, so geteuid() there also reads 0 — that is how
 * those tests get CAP_NET_RAW/CAP_NET_ADMIN for a private veth pair without any
 * real host root, and refusing all of them would refuse the very tests this
 * daemon is proven by. The two are told apart by /proc/self/uid_map: the HOST's
 * own (initial) user namespace maps identically, "0 0 4294967295" — the full
 * 32-bit range, because nothing remapped it — while `unshare -r` always narrows
 * that to a single line's length of 1 (one host uid, one namespace uid).
 * Measured directly: a live desk shell reads 4294967295; the same binary under
 * `unshare -r --map-root-user` reads 1, with or without a private net or pid
 * namespace alongside it. Only the first shape is refused. A read failure (no
 * /proc, or a kernel with no uid_map) proves nothing either way and does NOT
 * refuse — a preflight must never turn an unrelated read failure into blocking
 * a legitimate start. */
static int uid_map_is_full_host_range(void)
{
	FILE *f = fopen("/proc/self/uid_map", "r");
	if (!f)
		return 0;
	unsigned long long ns_id = 0, host_id = 0, len = 0;
	int got = (fscanf(f, "%llu %llu %llu", &ns_id, &host_id, &len) == 3);
	fclose(f);
	return got && ns_id == 0 && host_id == 0 && len == 4294967295ULL;
}

/* Forward-declared: defined near main() (push_libreac_tunables's own comment explains
 * why), used earlier by the per-listener setup below. */
static const char *resolve_clock_ref(void);
static void push_libreac_tunables(void);

static void refuse_if_root(void)
{
	if (geteuid() != 0)
		return;
	if (!uid_map_is_full_host_range())
		return;  /* uid 0 inside a mapped namespace: a test's fake root, or a
		          * rootless container — not the host's real root. */

	reac_code_emit(stderr, "reac-pw", RC_E_ROOT_REFUSED,
	    "FATAL — refusing to start as uid 0 (root).\n"
	    "         A root instance is never the console user's: it runs under a\n"
	    "         DIFFERENT systemd --user manager than the console session's,\n"
	    "         so it binds the SAME abstract segment-lock socket and either\n"
	    "         wins it — locking the console user's own daemon out — or\n"
	    "         loses it while looking healthy. It would also open raw REAC\n"
	    "         sockets and mint VLAN sub-interfaces the console user cannot\n"
	    "         then close.\n"
	    "         This package needs no root: file capabilities on the binary\n"
	    "         (cap_net_raw,cap_net_admin,cap_sys_nice) already grant what a\n"
	    "         console-user process needs. Run it as the console user:\n"
	    "           systemctl --user enable --now reac-pw\n"
	    "         (README.md). Refusing to start.\n");
	exit(1);
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

/* ---- naming a segment lock's holder (2026-09-18) --------------------------
 *
 * "grep <name> /proc/net/unix" tells an operator a lock is held; it does not
 * say by WHOM. Measured 2026-09-18: the console daemon's refusal named the
 * socket and nothing else, and the operator had to go find the rival process
 * by hand. Best-effort, three /proc reads chained, and silent (not fatal, not
 * even logged) the moment any of them cannot see what it is looking for —
 * most often permission: an unprivileged console daemon asking about a ROOT
 * holder cannot open that pid's /proc/<pid>/fd at all (DAC denies a non-owner,
 * non-root reader), which is exactly the shape of the defect this exists
 * against and precisely the case it cannot see into. The refusal message
 * still carries the socket name and the manual `grep` line unconditionally,
 * so a silent lookup here never loses information the caller already had. */

/* The segment lock's own name is already the exact string /proc/net/unix
 * prints for an abstract socket (reac_seglock.c: "@" + the abstract path), so
 * this is a straight line-by-line match on the last column, no reformatting. */
static int seglock_inode_of(const char *name, unsigned long *inode_out)
{
	FILE *f = fopen("/proc/net/unix", "r");
	if (!f)
		return 0;
	char line[512];
	int found = 0;
	/* header line first; every /proc/net/unix has exactly one */
	if (!fgets(line, sizeof line, f)) {
		fclose(f);
		return 0;
	}
	while (fgets(line, sizeof line, f)) {
		char num[64], ref[16], proto[16], flags[16], type[16], st[16], inode[32], path[300];
		int n = sscanf(line, "%63s %15s %15s %15s %15s %15s %31s %299s",
		               num, ref, proto, flags, type, st, inode, path);
		if (n == 8 && strcmp(path, name) == 0) {
			*inode_out = strtoul(inode, NULL, 10);
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

/* Which pid holds an open fd on socket inode `inode` — scanning every
 * /proc/<pid>/fd this reader has permission to open. A pid whose /fd we
 * cannot open (another uid, no CAP_SYS_PTRACE) is skipped, not an error: the
 * scan reports what it could see, and the caller already treats "found
 * nothing" as silence, not as proof the socket has no holder. */
static int pid_holding_socket(unsigned long inode, pid_t *pid_out)
{
	DIR *proc = opendir("/proc");
	if (!proc)
		return 0;
	char target[48];
	snprintf(target, sizeof target, "socket:[%lu]", inode);
	int found = 0;
	struct dirent *pe;
	while (!found && (pe = readdir(proc))) {
		if (pe->d_name[0] < '0' || pe->d_name[0] > '9')
			continue;
		char fddir[64];
		snprintf(fddir, sizeof fddir, "/proc/%s/fd", pe->d_name);
		DIR *fdd = opendir(fddir);
		if (!fdd)
			continue;
		struct dirent *fe;
		while ((fe = readdir(fdd))) {
			if (fe->d_name[0] == '.')
				continue;
			char fdpath[300], linkbuf[128];
			snprintf(fdpath, sizeof fdpath, "%s/%s", fddir, fe->d_name);
			ssize_t ln = readlink(fdpath, linkbuf, sizeof linkbuf - 1);
			if (ln < 0)
				continue;
			linkbuf[ln] = '\0';
			if (strcmp(linkbuf, target) == 0) {
				*pid_out = (pid_t)atoi(pe->d_name);
				found = 1;
				break;
			}
		}
		closedir(fdd);
	}
	closedir(proc);
	return found;
}

/* "pid N, user X (comm)" for a pid we could read /proc/<pid>/status of;
 * "pid N" alone if the Uid: line could not be read (still worth having: a pid
 * is enough to `ps` or `kill` it). getpwuid() failing (no NSS entry) falls
 * back to the raw uid rather than dropping the line. */
static void describe_pid(pid_t pid, char *out, size_t outlen)
{
	char path[64], line[256], comm[64] = "";
	uid_t uid = (uid_t)-1;
	snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
	FILE *f = fopen(path, "r");
	if (f) {
		while (fgets(line, sizeof line, f)) {
			if (strncmp(line, "Name:", 5) == 0)
				sscanf(line + 5, "%63s", comm);
			else if (strncmp(line, "Uid:", 4) == 0) {
				unsigned u;
				if (sscanf(line + 4, "%u", &u) == 1)
					uid = (uid_t)u;
			}
		}
		fclose(f);
	}
	if (uid == (uid_t)-1) {
		snprintf(out, outlen, "pid %d", (int)pid);
		return;
	}
	struct passwd *pw = getpwuid(uid);
	if (pw)
		snprintf(out, outlen, "pid %d, user %s (%s)", (int)pid, pw->pw_name,
		         comm[0] ? comm : "?");
	else
		snprintf(out, outlen, "pid %d, uid %u (%s)", (int)pid, (unsigned)uid,
		         comm[0] ? comm : "?");
}

/* The one entry point: `out` is "" (untouched callers just skip it) unless
 * every step above succeeded. */
static void describe_seglock_holder(const char *name, char *out, size_t outlen)
{
	out[0] = '\0';
	unsigned long inode;
	pid_t pid;
	if (seglock_inode_of(name, &inode) && pid_holding_socket(inode, &pid))
		describe_pid(pid, out, outlen);
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
	  "                cadence + return our inputs upstream) | tap (PASSIVE: serve what\n"
	  "                is heard and transmit NOTHING — no announce, join, grant or\n"
	  "                segment lock. One reac-capture node per heard stream: the\n"
	  "                master downstream as reac-capture.<segment>, each box as\n"
	  "                reac-capture.<segment>.<mac6>. Useful on a switch MIRROR port\n"
	  "                beside a real desk, where anything we transmit stops that\n"
	  "                desk's own box from enrolling.)\n"
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
	  "  --mixer M     master role: which desk NAME reac-pw logs as (m200|m300|m5000;\n"
	  "                default m200). Does not set the wire's pace-code byte — that comes\n"
	  "                from --rate alone; grants are box-defined so any box locks to any\n"
	  "                profile.\n"
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
	  "  REAC frame heard makes that interface a segment named after it, the ROLE is taken\n"
	  "  from what is heard on it, and link loss drops it after a %d s hold. Nothing names\n"
	  "  an interface in advance and NO environment variable decides a role.\n"
	  "auto-spine (ONE daemon, N listeners — 2026-08-20-reac-auto-spine.md §5): only the\n"
	  "  FIRST --live segment honours the per-box flags above. Every OTHER segment, and\n"
	  "  every heard one, reads its own settings from the layered conf, keyed by its name:\n"
	  "  REAC_<KEY>_<segment> in ~/.config/reac-pw/reac-pw.env above the bare REAC_<KEY>\n"
	  "  (reac_conf.h's precedence) -- for every key EXCEPT the role, which no layer here\n"
	  "  can answer any more.\n"
	  "the ONE override, ~/.config/reac-pw/reac-pw.conf (hand-written; nothing generates it):\n"
	  "    [segment IFNAME]\n"
	  "    role = auto|master|slave|tap     default: auto -- the wire decides\n"
	  "    ignore = yes                     never sniffed, served or minted\n"
	  "  Naming [segment <parent>.<vid>] also DECLARES that VLAN: its netdev is created,\n"
	  "  brought up and marked ours at START and whenever its parent appears, without\n"
	  "  waiting to hear a tagged frame on the trunk -- on a cold rig no box speaks until\n"
	  "  a master does, and the master needs the netdev first. The exit removes what it\n"
	  "  created. With no such file every segment is auto, which is the shipping default.\n"
	  "    REAC_TX=IFNAME             default: the same interface (this rig's masters\n"
	  "                               always tx == live)\n"
	  "    REAC_ROLE / REAC_ROLE_<segment>  RETIRED (2026-09-16). Read only to be NAMED\n"
	  "                               at start as ignored: a role written before there\n"
	  "                               was anything to decide against outlives the rig it\n"
	  "                               described. Use reac-pw.conf above.\n"
	  "    REAC_MIXER=m200|m300|m5000 default: m200\n"
	  "    REAC_NAME=NAME             node suffix; default: the interface name (the FIRST\n"
	  "                               --live segment defaults to bare names instead,\n"
	  "                               matching every invocation before this one)\n"
	  "    REAC_HEADAMP=\"CH:PARAM:VALUE ...\"  the head-amp re-assertion table, space or\n"
	  "                               comma separated (replaces N --headamp flags)\n"
	  "    REAC_BOX_CHANNELS=N        slave role: our own input width; default 16.\n"
	  "                               IGNORED under role = box, where the model row\n"
	  "                               is the only width; named as ignored at start.\n"
	  "    REAC_SRC_MAC=aa:bb:..      the source address on this wire (per-segment too)\n"
	  "  and REAC_RATE per segment exactly as a single-segment run already resolves it.\n"
	  "environment (see docs/ENV-KNOBS.md; unset = the default behaviour named below):\n"
	  "  REACPW_GRANT_ON_DECLARE=0  master role: opt OUT of ending the grant dwell on the\n"
	  "                box's declaration, restoring the full wall-clock hold. The dwell is\n"
	  "                a CAP for an undeclared box, not a wait (default: end on declare).\n"
	  "  REACPW_GRANT_DWELL_S=N  master role: set that CAP to N whole seconds (default:\n"
	  "                the built-in ~1.6 s; a real M-200 holds a cold box ~27 s)\n"
	  "  REAC_DEBUG=1  opt-in RX/source telemetry on stderr (~every 2 s: frame/dup/gap\n"
	  "                counters, ring fill, active channels)\n"
	  "  REAC_IFACES_ALLOW_WIRELESS=ifname[,ifname...]|*  autodetect: opt a wireless\n"
	  "                NIC INTO the scan (default: every wireless NIC excluded — Wi-Fi's\n"
	  "                jitter has no repacer here)\n"
	  "  REACPW_NO_ENROLL=1  master role: suppress the pre-grant ENROLL for a box whose\n"
	  "                width is already known (no real desk sends it to an S-1608). A\n"
	  "                RIG-TEST SWITCH, not a new default — see docs/ENV-KNOBS.md.\n"
	  "  REACPW_CLOCK_FOLLOW=0  master role: opt OUT of disciplining the TX cadence to\n"
	  "                the best available clock reference (NIC/external PHC > a\n"
	  "                hardware-driven PipeWire graph clock > the box's counter slope)\n"
	  "                and free-run on CLOCK_MONOTONIC instead. FOLLOWING IS THE\n"
	  "                DEFAULT (arbitration spec S3): the period is steered continuously\n"
	  "                and bounded, the phase is never stepped, the reference in use is\n"
	  "                printed on every change, and with none available we free-run and\n"
	  "                SAY so.\n"
	  "  REACPW_CLOCK_REF=<substring>  designate WHICH device is the clock reference\n"
	  "                (case-insensitive substring of the device name, e.g. 'Babyface').\n"
	  "                A designated device outranks the name heuristic; it does NOT\n"
	  "                rescue a structurally unusable one (HDMI/DisplayPort sinks,\n"
	  "                software timers) and it does NOT outrank measured instability.\n"
	  "                Only consulted while following (i.e. unless CLOCK_FOLLOW=0).\n"
	  "  REACPW_CATCHUP_MAX_SLOTS=<n>  master role: how many OVERSLEPT slots the\n"
	  "                pacer repays by staying on its grid instead of re-basing the\n"
	  "                phase and losing them. Unset = backend-dependent, because the\n"
	  "                two backends measure lateness against different references:\n"
	  "                thread = 1000 us of measured wake tail (4 slots at 4000 fps),\n"
	  "                etf = the LEAD less the qdisc delta (17 slots at 8000 fps on\n"
	  "                the default 2500 us lead), because everything inside the lead\n"
	  "                is repayable by construction. -1 = never repay.\n"
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

	/* FROM THE TABLE, so this list cannot drift from what reac_knobs_announce()
	 * and docs/ENV-KNOBS.md actually walk (operator ruling, 2026-09-17): every
	 * knob above is also settable at the command line, highest precedence. */
	fprintf(stderr,
	  "  --set KEY=VALUE  override any knob below at the highest precedence\n"
	  "                (cli > env > conf files > built-in default). Repeatable.\n"
	  "                Unknown KEY refuses to start (E_UNKNOWN_KNOB).\n"
	  "  Every knob --set and reac-pw.conf/.env accept (%d; see ENV-KNOBS.md for\n"
	  "  what each does and its default):\n", g_reac_knobs_count);
	for (int i = 0; i < g_reac_knobs_count; i++)
		fprintf(stderr, "    %s\n", g_reac_knobs[i].key);
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
	int                          pinned; /* --box: the nodes are the operator's statement
	                                      * about this wire and survive an absent box  */
	const char                  *tag;   /* "[iface] " once N>1, "" for a lone listener */
	/* The bounded rebuild ladder for a reac-capture that never reached the graph
	 * (reac_node_recover.h). Separate from `last` because a rebuild is not a model
	 * change and must not re-announce one. */
	struct reac_node_recover     recover;
	int                          restamp; /* a rebuilt peer needs the sink's badges again */
	const struct reac_box_model *announced;  /* the model the "autodetected" line named */
	/* THE WAKE LADDER (reac_wake.h, spec 2026-09-16-a-dropped-box-wakes-on-a-phy-edge).
	 * It lives on THIS timer because a master segment already has exactly one 200 ms
	 * main-loop poll and a second one would be a second opinion about the same wire.
	 * `ifname` is the device this master drives — the only one it may ever bounce — and
	 * an edge in flight is `up_at_ns`, so the down and the up are two turns of this
	 * timer and nothing ever sleeps in the loop. */
	const char             *ifname;
	struct reac_wake        wake;
	uint64_t                up_at_ns;   /* the link is down until here (0 = no edge) */
	int                     wake_open;  /* the ladder is open for this PROBING spell */
};

/* Other segments served over the same physical port — a bounce takes them all down with
 * it. Defined after the listener table it walks; declared here because the wake step is
 * on this timer and the table is a page further down. */
static int port_siblings_served(const char *ifname);

/* THE SEGMENT IS READY, SAID ONCE PER BOX AND ONLY WHEN IT IS TRUE. This line is what an
 * operator reads as "the nodes are there"; the node is CONNECTING for a moment after it
 * is built, so the announcement waits for it to be real and then never repeats — a
 * rebuild attempt has its own line and must not read as a fresh success. */
static void autodetect_announce(struct autodetect_ctx *c, const struct reac_box_model *bm)
{
	if (c->announced == bm || !reac_source_node_on_graph(*c->src, NULL))
		return;
	c->announced = bm;
	fprintf(stderr, "reac-pw: %sautodetected %s -> reac-capture %d in / reac-playback "
	        "%d out\n", c->tag, bm->display, bm->in_ch, bm->out_ch);
}

/* The ladder's clock. CLOCK_MONOTONIC, like every other deadline in this daemon; declared
 * here because main's own monotonic_ns() lives a page below the timer that needs it. */
static uint64_t wake_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ONE TURN OF THE WAKE LADDER. Everything it reads is already published by the pacer; the
 * only thing it can DO is one rtnetlink write on the device this master drives. The
 * decision itself is reac_wake and is not re-litigated here — this function is the eyes and
 * the hands, and it keeps no policy of its own. */
static void wake_step(struct autodetect_ctx *c)
{
	if (!c->sink || !c->ifname || !c->ifname[0])
		return;
	const uint64_t now = wake_now_ns();

	/* AN EDGE IN FLIGHT IS FINISHED BEFORE ANYTHING ELSE IS ASKED. A link left down
	 * because a later step decided something else would be the daemon breaking its own
	 * segment and calling it a remedy. */
	if (c->up_at_ns) {
		if (now < c->up_at_ns)
			return;
		c->up_at_ns = 0;
		if (reac_link_admin(c->ifname, 1) != 0)
			fprintf(stderr, "reac-pw: %sCOULD NOT BRING '%s' BACK UP after the wake "
			        "edge (errno %d). The segment is down until it is: "
			        "`ip link set %s up`.\n", c->tag, c->ifname, errno, c->ifname);
		else
			fprintf(stderr, "reac-pw: %s'%s' is back up — a box that had dropped sees "
			        "this as PHY LINK-UP and has %.0f s to flood, cold-connect and be "
			        "granted.\n", c->tag, c->ifname,
			        (double)REAC_WAKE_SETTLE_NS / 1e9);
		return;
	}

	struct reac_wake_obs o = { 0 };
	if (!reac_sink_node_wake_obs(c->sink, &o))
		return;                          /* no master engine on this segment */
	const int probing = o.probing;
	if (!c->wake_open) {
		reac_wake_init(&c->wake, now);
		c->wake_open = 1;
	} else if (probing && !c->wake.opened_ns) {
		reac_wake_reopen(&c->wake, now);
	}

	/* The two facts the ENGINE cannot know: what the kernel says about this cable, and
	 * what else this daemon is serving over the same port. */
	o.carrier = reac_link_carrier(c->ifname);
	o.siblings_served = port_siblings_served(c->ifname);
	/* A master that is granting or established re-opens the ladder's GRACE, so a box
	 * that enrols and drops again is pushed to before it is ever bounced again. The
	 * bounce COUNT deliberately survives it (reac_wake_reopen). */
	if (!probing)
		c->wake.opened_ns = 0;

	switch (reac_wake_step(&c->wake, now, &o)) {
	case REAC_WAKE_ACT_NONE:
		return;
	case REAC_WAKE_ACT_BOUNCE:
		fprintf(stderr, "reac-pw: %sthe box on '%s' has answered nothing through %llu "
		        "COMPLETED scene pushes with the carrier up. A box that has DROPPED "
		        "leaves that state on a PHY link-up and on nothing else, so this "
		        "master makes the edge itself: '%s' goes down for %u ms and back up "
		        "(edge %u of %u).\n",
		        c->tag, c->ifname, (unsigned long long)o.scene_pushes, c->ifname,
		        REAC_WAKE_DOWN_MS, c->wake.bounces, REAC_WAKE_MAX_BOUNCES);
		if (reac_link_admin(c->ifname, 0) != 0) {
			fprintf(stderr, "reac-pw: %sthe wake edge on '%s' was REFUSED (errno "
			        "%d)%s — nothing was touched and the box is still silent.\n",
			        c->tag, c->ifname, errno,
			        errno == EPERM ? ": this daemon has no CAP_NET_ADMIN, which the "
			                         "reac-pw RPM grants by file capability and a "
			                         "hand-started binary does not" : "");
			return;
		}
		c->up_at_ns = now + (uint64_t)REAC_WAKE_DOWN_MS * 1000000ULL;
		return;
	case REAC_WAKE_ACT_EXHAUSTED:
		fprintf(stderr, "reac-pw: %sthe box on '%s' did not answer %u PHY edges and is "
		        "still silent with the carrier up. This master has nothing left to try "
		        "and will not flap the port again. What is left is physical: unplug and "
		        "replug the box's REAC cable, or power-cycle the box. (A box that is "
		        "simply not on this segment looks exactly the same from here.)\n",
		        c->tag, c->ifname, REAC_WAKE_MAX_BOUNCES);
		return;
	}
}

static void on_autodetect_timer(void *data, uint64_t expirations)
{
	(void)expirations;
	struct autodetect_ctx *c = data;
	/* Whatever the RT callback queued for us — it never prints for itself. */
	reac_source_node_drain_log(*c->src, stderr);
	/* BEFORE the recognition gate, and that is the whole point: the silence this
	 * answers is a segment with NO box recognized, which is exactly the state the
	 * early return below leaves unattended. */
	wake_step(c);
	const struct reac_box_model *bm = reac_sink_node_recognized_box(c->sink);
	if (!bm) {
		/* AND THE BOX THAT LEFT TAKES ITS NODES WITH IT (operator, 2026-09-16). The
		 * master's own FSM clears recognized_box when the peer goes, so this NULL is
		 * the departure and this is the one place that sees it on the main loop. A
		 * node that outlives its box is the same `none / 0 in` row arriving by the
		 * other door — and worse, because a console would keep patching into it.
		 *
		 * A PINNED --box IS EXEMPT, and deliberately: the pin says this box BELONGS on
		 * this wire, and its whole purpose is that the patch survives a box that is
		 * not powered yet. Removing its nodes would be removing the pin. */
		if (c->last && !c->pinned) {
			fprintf(stderr, "reac-pw: %sthe box is gone — removing reac-capture and "
			        "reac-playback for this segment. They come back, sized to it, "
			        "when a box is recognized here again.\n", c->tag);
			reac_source_node_destroy(*c->src);
			*c->src = NULL;
			reac_sink_node_unpublish(c->sink);
			c->last = NULL;
			c->announced = NULL;
			c->restamp = 1;
			reac_node_recover_init(&c->recover);
		}
		return;                      /* nothing recognized yet */
	}
	if (bm == c->last) {
		/* SAME MODEL AS LAST POLL — so the only question left is whether the node we
		 * SAID we built is really there. Announcing a resize and never checking is
		 * how a segment can hold a playback node, a log line naming its capture
		 * width, and no capture node, for as long as nobody looks at the graph. */
		const char *why = "no node was ever created";
		int on_graph = reac_source_node_on_graph(*c->src, &why);
		/* Read BEFORE the step, which resets the ladder the moment the node is back. */
		int attempts = c->recover.attempts;
		switch (reac_node_recover_step(&c->recover, on_graph)) {
		case REAC_RECOVER_WAIT:
			/* A REBUILD THAT WORKED SAYS SO. Without this the journal reads
			 * "rebuilding it (attempt 1 of 5)" and then nothing at all, which is
			 * exactly what a still-broken segment reads like — the failure this whole
			 * path exists to stop being silent about, moved one line down. Only after
			 * an attempt: a node that was never missing has nothing to report. */
			if (on_graph && attempts > 0)
				fprintf(stderr, "reac-pw: %sreac-capture is back on the graph "
				        "(attempt %d) — this segment's input patches can be made "
				        "again.\n", c->tag, attempts);
			/* Healthy, inside the window, or already reported. The announcement lives
			 * here too: the node is CONNECTING when it is built, so "it is there" is
			 * only ever true on a later tick. */
			autodetect_announce(c, bm);
			return;
		case REAC_RECOVER_GIVE_UP:
			fprintf(stderr, "reac-pw: %sreac-capture for %s is STILL not on the graph "
			        "after %d rebuilds (%s) — giving up on it. This segment has no input "
			        "patches and nothing here will change that; the box's outputs are "
			        "unaffected. It is retried the moment the node appears or the box "
			        "is re-recognized.\n",
			        c->tag, bm->display, REAC_RECOVER_MAX_ATTEMPTS, why);
			return;
		case REAC_RECOVER_REBUILD:
			fprintf(stderr, "reac-pw: %sreac-capture is NOT on the graph %.1f s after it "
			        "was sized to %s (%s) — rebuilding it (attempt %d of %d). A segment "
			        "without its capture node has no input patches at all.\n",
			        c->tag, reac_node_recover_spent(&c->recover) * 0.2, bm->display, why,
			        c->recover.attempts, REAC_RECOVER_MAX_ATTEMPTS);
			/* FORCE IT. reac_source_node_ensure rebuilds on a CHANGE of width or
			 * label, and neither moved — the node it would compare against is the one
			 * that failed, at exactly the width we want. Tearing it down first is what
			 * makes the next ensure() build rather than agree. */
			reac_source_node_destroy(*c->src);
			*c->src = NULL;
			c->restamp = 1;   /* the new node starts blank; see below */
			break;
		}
	} else {
		/* A different box: a fresh ladder, and a line to announce it. */
		reac_node_recover_init(&c->recover);
	}
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
		reac_code_emit(stderr, "reac-pw", RC_E_SIZING,
		        "%scould not size reac-capture to %d ch (%s)\n",
		        c->tag, bm->in_ch, bm->display);
	if (reac_sink_node_ensure(c->sink, bm->out_ch, bm->display) != 0)
		reac_code_emit(stderr, "reac-pw", RC_E_SIZING,
		        "%scould not size reac-playback to %d ch (%s)\n",
		        c->tag, bm->out_ch, bm->display);
	/* A REBUILT CAPTURE NODE IS BLANK UNTIL SOMEBODY STAMPS IT. The sink's badge push
	 * fires on a CHANGE, and a rebuild changes nothing it watches — so the recovered
	 * node would carry box-model "none" and width "0x0" until the box next dropped,
	 * which is a segment that looks broken to every client that reads those props. */
	if (c->restamp) {
		c->restamp = 0;
		reac_sink_node_restamp_peer(c->sink);
	}
	autodetect_announce(c, bm);
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
	/* WHAT WAS ASKED FOR, and by whom — intent and observation are two facts. `role`
	 * above is what we present on the wire; these say whether anybody chose it.
	 * `role_pinned` is set only by a PER-SEGMENT answer — `REAC_ROLE_<iface>` or an
	 * explicit --role — which the wire never overrides; a BARE REAC_ROLE describes every
	 * segment on the host, so it is the launch floor and the hunt supersedes it. */
	enum reac_role_intent role_intent;
	enum reac_conf_layer role_layer;
	int role_pinned;
	const struct reac_mixer_profile *mixer;
	char name_buf[64];    /* same reasoning as tx_if_buf */
	const char *inst_name;              /* NULL = bare node names */
	uint8_t src_mac[6];
	int src_mac_set;
	int box_channels;                   /* SLAVE role: our own input width */
	/* THE ROW WE PRESENT AS, when this segment is pinned `role = box`
	 * (docs/design/specs/2026-09-17-the-daemon-can-be-a-box.md). NULL on every other
	 * role. It is the ONE source for what we declare: the enrolment frames, the
	 * upstream width, the head-amp strap, the two nodes' port counts and the roster's
	 * model all read it, and nothing re-derives any of them from a number. */
	const struct reac_box_model *box_model;
	/* WHAT IS MASTERING THIS WIRE, carried out of the hunt's verdict (0.5.1, DESIGN.md).
	 * `wire_channels` is the width of the stream this segment RECEIVES — 40 for a desk's
	 * downstream, the box's own width when a stagebox on M masters it — and it sizes the
	 * capture node as well as naming the rival kind in the published answer.
	 * `join_box_master` is that second case: a receive-only join, because a box on M runs
	 * no handshake at all and there is no grant for a slave engine to answer.
	 * `door_only` is the refusal: a wire pinned MASTER with a box mastering it, published
	 * as a door with no engine behind it so the refusal can be SEEN. */
	unsigned wire_channels;
	int join_box_master;
	int door_only;
	/* WHY THERE IS NOTHING BEHIND THE DOOR: a REFUSAL (above), or simply that this
	 * segment has NOTHING TO SERVE YET (arbitration §6 Q5, ANSWERED 2026-09-14,
	 * option C; plug-and-play §2/§4, lane 1). A VLAN pinned `tap` with a silent wire
	 * and a pinned master with no box both published NO NODE AT ALL, so the console
	 * had no `/reac/segment` row and the operator could not change the role of the
	 * very segment that needed one — measured on the rig 2026-09-14. The mechanism is
	 * `door_only`'s, not a second one; this field only says which sentence the door
	 * tells and which answer set it publishes (`reac.master.state=none` — nothing is
	 * mastering it as far as we can hear — against the refusal's `foreign`). */
	int door_vacant;
	/* REAC GEAR HAS BEEN HEARD ON THIS WIRE — the `S_SEGMENT_HEARD` fact, carried from
	 * the hunt into the open because the link-budget admission needs it and cannot ask
	 * a hunt (#107, auto-role amendment 2026-09-20). It is what separates a segment
	 * with something on it from one taken on the masterless licence, and only the first
	 * may take a neighbour's budget. 0 for a `--live` segment: nothing heard it. */
	int wire_heard;
	/* THE PASSIVE ROLE (openmixer master-arbitration, eighth amendment, 2026-09-13).
	 * `REAC_ROLE_<segment>=tap` or `--role tap`: serve what is heard and TRANSMIT
	 * NOTHING — no announce, no join, no grant, no seglock, no TX socket. It is not a
	 * value of `role` above because `enum reac_role` is the WIRE's two ends and a tap
	 * presents neither; it rides beside it, gated by reac_role_intent_transmits. */
	int tap;
	uint8_t rival_mac[6];
	int rival_mac_set;
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

	/* THE TAP ENGINE — one passive receiver, one source node per heard stream. Its
	 * own rings and feeders live inside `tap` (reac_tap.h), which is why none of the
	 * single-stream fields above are used in this role: a tap serves N streams and
	 * the listener's `ring`/`rx`/`src` trio can hold one. */
	struct reac_tap tap;
	int tap_open;
	struct reac_source_node *tap_src[REAC_TAP_MAX_STREAMS];
	struct reac_source_node_cfg tap_src_cfg[REAC_TAP_MAX_STREAMS];
	char tap_inst[REAC_TAP_MAX_STREAMS][IFNAMSIZ + 16];

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

	/* HOW LONG THIS SEGMENT HAS FOLLOWED NOBODY (#97). `heard` above says whether the
	 * master's frames are still arriving and decoding; this counts the 200 ms publish
	 * ticks since it last did. It is the SEGMENT's own evidence, and it is the only
	 * evidence that answers the question: the sniffer's discovery table cannot tell an
	 * absent master from an enrolment in progress (a box master's stream is mostly
	 * filler, which the peer lock refuses as a sighting), and deciding from it destroyed
	 * a live box-master join — measured, tests/box-master-slave-join.sh. */
	int follows_nobody_ticks;

	struct autodetect_ctx adc;
	struct spa_source *ad_timer;
};

static void listener_cfg_defaults(struct listener_cfg *c)
{
	memset(c, 0, sizeof *c);
	c->rxcfg.pcap_realtime = 1;
	/* NOTHING CONFIGURED MEANS `auto`, not master: a parameter a normal box needs
	 * hand-set is a defect in the defaults, and the role is no more exempt from that
	 * than the rate is. The wire role still starts at master because the field holds one
	 * of two values; what it is on a heard segment is decided by reac_hunt before
	 * anything is transmitted. */
	c->role_intent = REAC_ROLE_INTENT_AUTO;
	c->role_layer = REAC_CONF_NONE;
	c->role_pinned = 0;
	c->role = REAC_ROLE_MASTER;
	c->mixer = reac_mixer_profile_by_name("m200");
	c->box_channels = REAC_SLAVE_BOX_CHANNELS_DEFAULT;
	/* A desk's downstream until the wire says otherwise: it is what every slave segment
	 * received before a box could master one, and it is what the RX gate accepts. */
	c->wire_channels = REAC_MAX_CHANNELS;
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

	/* REAC_ROLE, and WHICH KEY answered it. A per-segment `REAC_ROLE_<iface>` is a
	 * decision about THIS wire and is obeyed; a bare REAC_ROLE cannot know what is on
	 * one particular segment, so it is the launch floor the hunt resolves against
	 * — a console generating this file writes the bare key as the launch role for a
	 * segment it has not seen. `auto` at either level asks for the hunt outright. */
	/* THE ROLE COMES FROM THE OVERRIDE FILE OR FROM NOWHERE (spec §2, §3). Every layer
	 * reac_conf reads is silent on this question now — a launch-time key is a decision
	 * taken before there is anything to decide against, and it outlives the rig it
	 * described. With nothing said the segment is `auto` and the hunt elects it from
	 * the wire, which is what this daemon is for. */
	{
		reac_segconf_refresh(&g_segconf);
		enum reac_role_intent i;
		if (reac_segconf_role(&g_segconf, iface, &i)) {
			c->role_layer = REAC_CONF_SEGMENT;
			c->role_intent = i;
			c->role = reac_role_from_intent(i);
			c->role_pinned = (i != REAC_ROLE_INTENT_AUTO);
			c->tap = (i == REAC_ROLE_INTENT_TAP);
		}
		/* `role = box` — WE ARE THE STAGEBOX on this wire. The row comes from the
		 * same file, already validated there (reac_segconf's cross-check refuses a
		 * box with no model and falls the segment back to auto), so a row here is a
		 * row that exists. The upstream width is the ROW'S and never a second
		 * number: an output-only row still speaks at the minimum pair. */
		if (c->role_intent == REAC_ROLE_INTENT_BOX) {
			const char *tok = reac_segconf_model(&g_segconf, iface);
			c->box_model = tok ? reac_box_model_by_token(tok) : NULL;
			if (c->box_model) {
				c->box_channels = reac_box_model_upstream_width(c->box_model);
				/* ONE WORD FOR ONE SEGMENT (2026-09-17). The roster, the
				 * conf key and this line all say `box`; `BOX role`,
				 * `SLAVE role` and `slave` were three spellings of the
				 * same segment in one log. */
				fprintf(stderr, "reac-pw: [%s] role = box — presenting %s to the "
				        "mixer: %d in / %d out, firmware %u.%03u, REAC %u.%u%02u"
				        "%s\n", iface, c->box_model->display,
				        c->box_model->in_ch, c->box_model->out_ch,
				        c->box_model->fw_milli / 1000u, c->box_model->fw_milli % 1000u,
				        c->box_model->reac_major, c->box_model->reac_minor,
				        c->box_model->reac_patch,
				        c->box_model->origin == REAC_BOX_DERIVED
				                ? " (a DERIVED row — no real box of this model has"
				                  " ever been captured)" : "");
			} else {
				/* Unreachable through the conf, which refuses this case by name;
				 * kept because a silent fall-through to a default width is
				 * exactly the defect the refusal exists for. */
				fprintf(stderr, "reac-pw: [%s] role = box with no usable model — "
				        "the segment stays on the hunt\n", iface);
				c->role_intent = REAC_ROLE_INTENT_AUTO;
				c->role_pinned = 0;
				c->role = REAC_ROLE_MASTER;
			}
		}
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

	/* THE SOURCE ADDRESS, PER SEGMENT, WITHOUT A COMMAND LINE (0.5.6-4). `--src-mac`
	 * has always existed and the packaged daemon takes no arguments, so on a real rig
	 * there was no way to try a different source on one wire — and the box-master
	 * enrolment is exactly the experiment that needs one (docs/ENV-KNOBS.md). Same
	 * layered lookup as every other key, so `REAC_SRC_MAC_<segment>` answers for one
	 * wire and a bare `REAC_SRC_MAC` is the floor. */
	if (reac_conf_lookup("REAC_SRC_MAC", iface, NULL, v, sizeof v) != REAC_CONF_NONE) {
		unsigned b[6];
		if (sscanf(v, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
			for (int i = 0; i < 6; i++)
				c->src_mac[i] = (uint8_t)b[i];
			c->src_mac_set = 1;
		} else {
			fprintf(stderr, "reac-pw: [%s] ignoring malformed REAC_SRC_MAC='%s' "
			        "(aa:bb:cc:dd:ee:ff)\n", iface, v);
		}
	}

	/* THE ROW IS THE ONLY WIDTH A BOX HAS (spec 2026-09-17 §2a, §5: `in_ch`/`out_ch`
	 * are the ONE place a width is declared, and the PipeWire pair's widths come from
	 * the ROW and from nothing else). An env key that moved this number would put a
	 * second width in the daemon: the frames keep the row's geometry and everything
	 * sized from `box_channels` follows the env — wire and graph split, with no line
	 * saying so. So under `role = box` the key is READ ONLY TO BE NAMED, exactly as
	 * REAC_ROLE is (segment_say_env_role_retired): a key that stopped applying and
	 * says nothing is indistinguishable from one that is working. */
	{
		enum reac_conf_layer bcl =
			reac_conf_lookup("REAC_BOX_CHANNELS", iface, NULL, v, sizeof v);
		if (bcl != REAC_CONF_NONE && c->role_intent == REAC_ROLE_INTENT_BOX) {
			fprintf(stderr, "reac-pw: [%s] REAC_BOX_CHANNELS='%s' in %s is IGNORED "
			        "under role = box: the width is the model row's (%s: %d in / "
			        "%d out) and there is no second one (spec 2026-09-17 §2a, §5)\n",
			        iface, v, reac_conf_layer_name(bcl),
			        c->box_model ? c->box_model->display : "no row",
			        c->box_model ? c->box_model->in_ch : 0,
			        c->box_model ? c->box_model->out_ch : 0);
		} else if (bcl != REAC_CONF_NONE) {
			int n = atoi(v);
			if (n >= 2 && n <= REAC_MAX_CHANNELS && (n & 1) == 0)
				c->box_channels = n;
			else
				fprintf(stderr, "reac-pw: [%s] ignoring invalid "
				        "REAC_BOX_CHANNELS='%s'\n", iface, v);
		}
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
	char role_s[4];
	snprintf(role_s, sizeof role_s, "%d", REAC_CFG_ROLE_VALUE_SLAVE);

	/* THE SEGMENT AGGREGATE, from what this engine actually witnessed. The
	 * evidence is the RX's accepted-frame count: the gate passes ONE geometry —
	 * the 40-channel downstream when a desk masters the wire, the box's own width
	 * when a stagebox on M does (0.5.1) — so a moving count is both "a master is
	 * here" and "it is of the kind cfg.wire_channels says", the same two facts a
	 * master's arbitration derives from its sighting table, through the SAME
	 * classifier rather than a second one. */
	struct reac_segment_answer answer;
	if (L->cfg.door_only) {
		/* A door runs no engine at all, so the slave predicate's DOWN is the truth
		 * about it — the state is derived here rather than shared with the joined
		 * path below, which derives its own from the engine it actually has. */
		const char *state = reac_role_swap_state(
			&L->role_swap, reac_role_engine_of_slave(L->slave_open, established));
		/* A VACANT DOOR PUBLISHES AN ABSENCE, AND ABSENCE IS A FACT (Q5, option C).
		 * Nothing has been heard mastering this wire, so the slave composer's own
		 * `heard = 0` arm is exactly the answer: master.state `none`, master.mac
		 * `none`, rival.kind `none`, refusal `none`, pace `free-run`. No new
		 * vocabulary — the honest reading of every field this segment already has.
		 *
		 * THE ROLE STATE IS `role_hunting`, NOT `role_reestablish_pending`. Nothing
		 * is owed here and no swap has failed: this is a segment whose role cannot
		 * be PERFORMED because there is nothing on the wire to perform it against,
		 * which is precisely what reac_role_swap.h defines that word for. */
		if (L->cfg.door_vacant) {
			reac_segment_answer_slave(&answer, 0, 0, L->rx.sample_rate, 0);
			reac_source_node_publish_segment(L->src, role_s, REAC_ROLE_STATE_HUNTING,
			                                 reac_role_refuse_code(REAC_ROLE_REFUSE_NONE),
			                                 &answer);
			return;
		}
		/* A REFUSED SEGMENT PUBLISHES THE REFUSAL AND NOTHING ELSE (0.5.1). No engine
		 * runs here, so there is no frame count to latch on and no MAC we learned from
		 * a handshake — the rival's address comes from the sighting that caused the
		 * refusal, which is the only evidence there is. */
		reac_segment_answer_refused(&answer, L->cfg.wire_channels,
		                            L->cfg.rival_mac_set
		                              ? reac_mac48_pack(L->cfg.rival_mac) : 0,
		                            L->rx.sample_rate);
		reac_source_node_publish_segment(L->src, role_s, state,
		                                 reac_role_refuse_code(REAC_ROLE_REFUSE_NONE),
		                                 &answer);
		return;
	}
	int heard = reac_segment_heard_step(
		&L->heard,
		L->rx_started ? atomic_load_explicit(&L->rx.frames_ok, memory_order_relaxed) : 0,
		REAC_SEGMENT_HEARD_QUIET_TICKS);
	/* WHOSE CLOCK THIS SEGMENT IS ON. Normally the slave FSM learns it from the grant
	 * burst; a box that masters the wire GRANTS NOTHING (it runs no handshake at all),
	 * so for that join the address is the one the sighting carried — the same evidence
	 * the verdict itself was made from, and the only one there is. */
	uint64_t master_mac48 = L->slave_open
		? atomic_load_explicit(&L->slave.master_mac48, memory_order_relaxed) : 0;
	if (master_mac48 == 0 && L->cfg.join_box_master && L->cfg.rival_mac_set)
		master_mac48 = reac_mac48_pack(L->cfg.rival_mac);
	reac_segment_answer_slave(&answer, heard, master_mac48,
	                          L->rx.sample_rate, L->cfg.wire_channels);

	/* WHICH ENGINE ANSWERS FOR THE ROLE — and since 0.5.6 there is only one answer,
	 * because a box-master join runs the SAME slave engine a desk-master join does.
	 * 0.5.2 derived it from the RX instead, on the reasoning that a receive-only join
	 * IS the slave role performed; that reasoning went with the receive-only join. */
	const char *state = reac_role_swap_state(
		&L->role_swap, reac_role_engine_of_slave(L->slave_open, established));

	reac_source_node_publish_segment(L->src, role_s, state,
	                                 reac_role_refuse_code(REAC_ROLE_REFUSE_NONE),
	                                 &answer);

	/* AND THE BOX IS THE SAME BOX WHICHEVER END SENDS THE CLOCK. A console keys a
	 * stagebox off this identity set; a join that published none of it left the rig
	 * rendering a segment with no device on it and eight inputs nobody could patch
	 * (2026-09-09 05:45). The width is the wire's own declaration and the address is the
	 * sighting's — the only two facts a peer that runs no handshake ever gives us. */
	/* AND THE LAMP IS THE PAIRING (0.5.6, operator ruling). `heard` says frames are
	 * arriving and decoding, which is a fact about the WIRE; `reac.link-state` is what a
	 * console keys a stagebox off, and it must say whether we are JOINED. The rig,
	 * 2026-09-09: "S-0808 is not enrolled but omx sees it available", with the box's own
	 * lamp unlocked beside an S-1608's locked one. So the engine's ESTABLISHED flag
	 * answers it — probing through the flood and the wait for the grant echo,
	 * established once the unicast stream and the heartbeat are running. The rest of
	 * the identity set is unchanged: the width, the address and the model are facts
	 * about the wire and stay true while we are only listening to it. */
	if (L->cfg.join_box_master) {
		reac_source_node_publish_box_master(L->src, L->cfg.wire_channels,
		                                    master_mac48, established);
		/* AND THE SEGMENT'S OTHER DOOR SAYS THE SAME. A console folds the two nodes
		 * into one row, so a capture reading `established` beside a playback still at
		 * `probing` reads as a resync in progress — measured on the rig the moment the
		 * engine enrolled. Same composer, same values, same instant. */
		reac_sink_node_publish_box_master(L->sink, L->cfg.wire_channels,
		                                  master_mac48, established);
	}
}

/* Bring one segment online: resolve its rate, open the RX feeder, and (role
 * permitting) the TX side + node lifecycle — the single-instance body main()
 * used to run inline, called once per configured interface against ONE
 * shared loop. Returns 0 with L fully populated (still needs
 * reac_rx_start()), or -1 on a refusal this segment cannot recover from
 * (already reported on stderr, and everything this call opened is already
 * cleaned up). A refusal here does not necessarily end the daemon — see
 * main()'s single-vs-multi distinction at the call site. */
/* --- THE TAP ROLE: serve what is heard, transmit nothing ---------------------
 *
 * openmixer master-arbitration, eighth amendment (2026-09-13). Everything the other
 * roles do to the wire is ABSENT here and absent by construction, not by a flag that
 * could be read the wrong way: no reac_tx, no reac_pacer, no reac_slave, no
 * reac_seglock, no reac-playback sink. The whole receive path is reac_tap
 * (<reac/transport/reac_tap.h>), which classifies the segment and hands back one ring
 * and one feeder per heard stream.
 *
 * WHAT IT PUBLISHES. One reac-capture node per stream:
 *
 *   reac-capture.<segment>              the desk's 40-channel downstream
 *   reac-capture.<segment>.<mac6>       one per box source MAC, at the box's own width
 *
 * and every one of them carries reac.segment = <segment>, because they are one
 * segment's nodes and a console keys a stagebox off that identity. The NAME is an
 * address, the SEGMENT is an identity (reac_source_node_cfg.segment).
 *
 * WHICH NODE ANSWERS FOR THE SEGMENT: the master stream's, and only it. Two nodes
 * publishing the same segment's state would be two doors onto one fact.
 */

/* The short form of a box's MAC for a node name: the last three octets, which is what
 * an operator reads off a Roland chassis and what distinguishes two boxes on one
 * segment. Not an identity — reac.box-mac carries the whole address. */
static void tap_mac_short(char *out, size_t cap, const uint8_t mac[6])
{
	snprintf(out, cap, "%02x%02x%02x", mac[3], mac[4], mac[5]);
}

/* Publish the segment's answer from the tap's own evidence. A tap has no engine and no
 * handshake, so the two facts it has are the MASTER STREAM (its source MAC and that its
 * frames are arriving) and the measured rate.
 *
 * `master_state` is DERIVED, not asserted. The amendment says a tap reads `foreign`, and
 * it does — for as long as the master is being heard. Publishing the string
 * unconditionally would leave a desk that was unplugged reading `foreign` forever, and
 * absence is a fact this codebase publishes rather than hides (reac_segment_heard). */
static void listener_publish_tap(struct listener *L)
{
	const struct reac_tap_stream *m = reac_tap_survey_master(&L->tap.survey);
	struct reac_source_node *door = NULL;
	for (unsigned i = 0; i < L->tap.n; i++)
		if (L->tap.survey.stream[i].kind == REAC_TAP_STREAM_MASTER) {
			door = L->tap_src[i];
			break;
		}
	if (!door || !m)
		return;

	int heard = reac_segment_heard_step(
		&L->heard,
		atomic_load_explicit(&L->tap.rx[0].frames_ok, memory_order_relaxed),
		REAC_SEGMENT_HEARD_QUIET_TICKS);

	struct reac_segment_answer answer;
	reac_segment_answer_slave(&answer, heard, reac_mac48_pack(m->src),
	                          L->tap.sample_rate, m->channels);
	char role_s[4];
	snprintf(role_s, sizeof role_s, "%d", REAC_CFG_ROLE_VALUE_SLAVE);
	reac_source_node_publish_segment(door, role_s, REAC_ROLE_STATE_TAP,
	                                 reac_role_refuse_code(REAC_ROLE_REFUSE_NONE),
	                                 &answer);
}

static void listener_close_tap(struct listener *L)
{
	for (unsigned i = 0; i < REAC_TAP_MAX_STREAMS; i++) {
		reac_source_node_destroy(L->tap_src[i]);
		L->tap_src[i] = NULL;
	}
	if (L->tap_open) {
		reac_tap_close(&L->tap);
		L->tap_open = 0;
	}
}

/* listener_open_tap's third answer, beside 0 and -1: the source opened and carried no
 * REAC at all, so there is nothing to serve YET. Not a failure — the segment exists and
 * gets its door (see listener_open). */
#define LISTENER_TAP_VACANT 1

static int listener_open_tap(struct listener *L, struct pw_loop *loop)
{
	struct listener_cfg *c = &L->cfg;

	uint8_t self[6];
	int have_self = c->rxcfg.kind == REAC_RX_LIVE &&
	                reac_mac_default_src(c->rxcfg.source, self) == 0;

	struct reac_tap_cfg tcfg = {
		.kind = c->rxcfg.kind,
		.source = c->rxcfg.source,
		/* A TAP NEVER FORCES THE RATE FROM A FILE. It locks to the master's cadence
		 * the way a slave does; an explicit --rate still wins, because that is an
		 * operator saying what the wire is. */
		.forced_rate = c->rate_layer == REAC_CONF_ARGV ? c->rxcfg.forced_rate : 0,
		.survey_ms = 1000,
		.self_mac = have_self ? self : NULL,
	};
	if (reac_tap_open(&L->tap, &tcfg) != 0) {
		fprintf(stderr, "reac-pw: %sTAP could not open '%s'\n", c->tag, c->rxcfg.source);
		return -1;
	}
	L->tap_open = 1;
	if (L->tap.n == 0) {
		fprintf(stderr, "reac-pw: %sTAP heard NOTHING on '%s' in 1 s — no master, no "
		        "box. A tap serves what is on the wire and there is nothing to serve; "
		        "on a venue switch this is the mirror port not being configured. The "
		        "segment is published as a VACANT DOOR so it can be seen and its role "
		        "set, and the tap opens for real on the first frame heard.\n",
		        c->tag, c->rxcfg.source);
		listener_close_tap(L);
		return LISTENER_TAP_VACANT;
	}

	for (unsigned i = 0; i < L->tap.n; i++) {
		const struct reac_tap_stream *st = &L->tap.survey.stream[i];
		const char *seg = c->inst_name && *c->inst_name ? c->inst_name : c->rxcfg.source;
		if (st->kind == REAC_TAP_STREAM_MASTER) {
			snprintf(L->tap_inst[i], sizeof L->tap_inst[i], "%s", seg);
		} else {
			char shortmac[8];
			tap_mac_short(shortmac, sizeof shortmac, st->src);
			snprintf(L->tap_inst[i], sizeof L->tap_inst[i], "%s.%s", seg, shortmac);
		}
		L->tap_src_cfg[i] = (struct reac_source_node_cfg){
			.loop = loop, .ring = &L->tap.ring[i], .rx = &L->tap.rx[i],
			.sample_rate = L->tap.sample_rate,
			.inst = L->tap_inst[i],
			.segment = seg,
			/* NOT a master badge: a tap probes nothing, so the create-time
			 * "probing" link-state a master stamps would be a claim about a
			 * handshake it never runs. */
			.master_role = 0,
			.clock_ref = NULL,
		};
		if (reac_source_node_ensure(&L->tap_src[i], &L->tap_src_cfg[i],
		                            (int)st->channels, NULL) != 0) {
			fprintf(stderr, "reac-pw: %sTAP could not create reac-capture.%s\n",
			        c->tag, L->tap_inst[i]);
			listener_close_tap(L);
			return -1;
		}
		fprintf(stderr, "reac-pw: %sTAP serving %s as reac-capture.%s (%u ch, "
		        "%02x:%02x:%02x:%02x:%02x:%02x)\n", c->tag,
		        st->kind == REAC_TAP_STREAM_MASTER ? "the master's downstream"
		                                           : "a box's return",
		        L->tap_inst[i], st->channels,
		        st->src[0], st->src[1], st->src[2], st->src[3], st->src[4], st->src[5]);
	}

	if (reac_tap_start(&L->tap) != 0) {
		fprintf(stderr, "reac-pw: %sTAP feeders would not start\n", c->tag);
		listener_close_tap(L);
		return -1;
	}
	reac_segment_heard_init(&L->heard, 0);
	listener_publish_tap(L);
	fprintf(stderr, "reac-pw: %sTAP up at %d Hz over %u stream(s) — NOTHING is "
	        "transmitted on this segment: no announce, no join, no grant, no segment "
	        "lock, and no TX socket was opened\n",
	        c->tag, L->tap.sample_rate, L->tap.n);
	return 0;
}

/* THE LINK'S SPEED IN Mbit/s, or 0 when it cannot be read — a down port, a veth, a
 * netns where sysfs is not ours. `reac_link_budget_fits` treats 0 as "no limit
 * known", never as a refusal (reac_link_budget.h). */
static unsigned link_speed_mbit(const char *ifname)
{
	char parent[IFNAMSIZ], path[64 + IFNAMSIZ];
	uint16_t vid;
	/* A VLAN sub-interface has no speed of its own: the wire is the PARENT's, and so
	 * is the budget every segment on that trunk shares. */
	const char *dev = reac_declared_vlan_split(ifname, parent, sizeof parent, &vid)
	                  ? parent : ifname;
	/* AND THE OPERATOR MAY DECLARE IT (#107, auto-role amendment 2026-09-20). sysfs
	 * reports what the PHY negotiated, which is not always what the wire carries — a
	 * 1 Gbit NIC into a 100 Mbit uplink — and in a network namespace it reports nothing
	 * at all while a veth answers 10 Gbit, so the admission below is untestable without
	 * this. Resolved through the knob table, so `--set` and every conf layer reach it
	 * and `reac_knobs_announce` prints it at start; per PORT, never per VLAN. */
	char declared[32];
	if (reac_knobs_resolve_port("REACPW_LINK_MBIT", dev, declared, sizeof declared)
	    != REAC_CONF_NONE) {
		long d = strtol(declared, NULL, 10);
		/* Out of range is UNKNOWN, which never refuses — never a silent clamp to
		 * some plausible-looking rate. */
		return (d > 0 && d <= 1000000) ? (unsigned)d : 0;
	}
	snprintf(path, sizeof path, "/sys/class/net/%s/speed", dev);
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	long v = 0;
	int got = fscanf(f, "%ld", &v);
	fclose(f);
	return (got == 1 && v > 0) ? (unsigned)v : 0;
}

/* The PHYSICAL port a segment transmits on — its VLAN parent, or itself. Two segments
 * share a budget exactly when this answers the same name for both. */
static void link_port_of(const char *ifname, char *out, size_t cap)
{
	char parent[IFNAMSIZ];
	uint16_t vid;
	if (reac_declared_vlan_split(ifname, parent, sizeof parent, &vid))
		snprintf(out, cap, "%s", parent);
	else
		snprintf(out, cap, "%s", ifname);
}

/* What the OTHER master segments on this listener's physical port have already
 * committed, in kbit/s — defined beside the listener table it walks (this daemon's ONE
 * `struct hearing`), declared here because listener_open is what asks. */
static uint64_t link_used_kbit(const struct listener *self);

/* WHO HOLDS THIS LISTENER'S PORT, and what each holder is CARRYING — the refusal's own
 * sentence, defined beside the table it walks for the same reason as above. #107: the
 * 690 refusals of 2026-09-20 named the port and the kbit/s and left the holder to a
 * roster dump, on a rig where the holder was an empty VLAN. */
static void link_budget_holders(const struct listener *self, char *out, size_t cap);

/* THE EVICTION THE ADMISSION BELOW ASKS FOR BEFORE IT REFUSES (#107). Defined beside
 * hearing_drop, which is what it acts through; returns how many segments yielded. */
static int link_budget_yield(const char *taker, const char *tx_if, uint64_t now);
static uint64_t monotonic_ns(void);

/* ONE OWNER PER SEGMENT'S PAIR, AND IT IS THIS LISTENER (#108, autodetect spec amendment
 * 2026-09-20 §a). Every node this listener holds, destroyed and forgotten — the capture,
 * the playback, and a tap's per-stream sources, because a tap listener's pair lives in
 * `tap_src[]` and nowhere else.
 *
 * NULLING A POINTER IS NOT A TEARDOWN, which is the whole defect: a dropped handle leaves
 * the pw_stream CONNECTED, so the pair stays on the graph for the life of the process,
 * carrying `reac.box-model=none` / `reac.box-width=0x0` on the SAME `reac.segment` as the
 * real pair — the ghost openmixer's segment scan read instead of the established S-1608.
 *
 * `loop` is needed for the autodetect timer, which must stop BEFORE the nodes it rebuilds
 * go away (listener_close has always done it in this order and for this reason). */
static void listener_drop_nodes(struct listener *L, struct pw_loop *loop)
{
	if (L->ad_timer) {
		pw_loop_destroy_source(loop, L->ad_timer);
		L->ad_timer = NULL;
	}
	reac_source_node_destroy(L->src);          /* NULL-safe by contract */
	L->src = NULL;
	reac_sink_node_destroy(L->sink);
	L->sink = NULL;
	for (unsigned i = 0; i < REAC_TAP_MAX_STREAMS; i++) {
		reac_source_node_destroy(L->tap_src[i]);
		L->tap_src[i] = NULL;
	}
}

/* Does this listener hold anything the graph can see? Read before an open and after a
 * failed one: a listener that answers yes at either point is a ghost in the making. */
static int listener_holds_nodes(const struct listener *L)
{
	if (L->src || L->sink)
		return 1;
	for (unsigned i = 0; i < REAC_TAP_MAX_STREAMS; i++)
		if (L->tap_src[i])
			return 1;
	return 0;
}

static int listener_open(struct listener *L, struct pw_loop *loop)
{
	struct listener_cfg *c = &L->cfg;

	/* A LISTENER NEVER FORGETS A PAIR. This used to be `L->src = NULL; L->sink = NULL;`,
	 * which is how a re-open with no close in front of it minted a second pair beside a
	 * first that nothing could ever destroy. Reaching this with nodes in hand is a CALLER
	 * that skipped listener_close, so it is destroyed AND named — a silent tidy-up here
	 * would hide the caller that needs fixing. */
	if (listener_holds_nodes(L)) {
		reac_code_emit(stderr, "reac-pw", RC_E_ORPHAN_PAIR,
		    "%sopening a segment that still holds its previous node pair — it is "
		    "destroyed here rather than dropped, but a re-open reached this without a "
		    "close in front of it and that is the bug to fix\n", c->tag);
		listener_drop_nodes(L, loop);
	}
	L->src = NULL;
	L->sink = NULL;
	L->slave_open = 0;
	L->tx_ring_init = 0;
	reac_seglock_init(&L->seglock);
	L->ad_timer = NULL;

	/* THE PASSIVE ROLE TAKES NONE OF THE PATH BELOW. Every line after this point
	 * opens a socket, claims a lock or starts an engine, and a tap does none of the
	 * three — so it branches here rather than threading an `if (!tap)` through the
	 * whole function, where one missed arm would be a frame on a desk's wire. */
	if (c->tap) {
		int r = listener_open_tap(L, loop);
		if (r != LISTENER_TAP_VACANT)
			return r;
		/* NOTHING ON THE MIRROR YET, AND THE SEGMENT STILL EXISTS (Q5, option C).
		 * Until now a tap that heard nothing FAILED to open, the listener was wiped,
		 * and a VLAN pinned `tap` on a quiet wire published no node — so the console
		 * had no row for it and its role could not be changed back (measured
		 * 2026-09-14). A tap is pinned by an operator who knows the mirror is coming;
		 * the door is what carries that intent until it does. It falls through into
		 * the SAME door the 0.5.1 refusal uses — one door in this daemon, not two —
		 * and the kept sniffer replaces it with the real tap on the first verdict. */
		c->door_only = 1;
		c->door_vacant = 1;
		c->wire_channels = 0;
	}

	/* SAMPLE RATE — the master chooses it; the box follows. See
	 * docs/RATE-AND-CLOCK-CONFIG.md for the full law; this is its per-segment
	 * application, unchanged from the single-instance code it replaces. */
	if ((c->role == REAC_ROLE_MASTER || c->door_only) && c->rxcfg.forced_rate == 0)
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
	 * broadcast. The wire carries both; the gate keeps them apart.
	 *
	 * AND A BOX THAT MASTERS THE WIRE IS THE THIRD CASE (0.5.1): it broadcasts its
	 * own UPSTREAM geometry — `52 + n*36`, never a master downstream frame
	 * (reac-protocol/wire-format.md) — so the segment reads it through the SAME
	 * box-width gate and the SAME decoder the master role already uses for a box's
	 * return. No second decoder, and no new frame kind. */
	c->rxcfg.accept = (c->role == REAC_ROLE_MASTER || c->join_box_master)
	                    ? REAC_RX_ACCEPT_UPSTREAM : REAC_RX_ACCEPT_DOWNSTREAM;

	if (reac_rx_open(&L->rx, &c->rxcfg, &L->ring) != 0) {
		fprintf(stderr, "reac-pw: %scannot open source '%s'\n", c->tag, c->rxcfg.source);
		return -1;
	}
	fprintf(stderr, "reac-pw: %srecovered REAC rate = %d Hz (%d pps), rx stream = %s\n",
	        c->tag, L->rx.sample_rate, L->rx.sample_rate / REAC_SAMPLES_PER_PKT,
	        c->join_box_master ? "a box master's own broadcast (box-width)"
	        : c->rxcfg.accept == REAC_RX_ACCEPT_UPSTREAM
	          ? "box upstream return (box-width)" : "master downstream (40 ch)");

	/* The reac-capture source is created AFTER the TX side, because whether to DEFER
	 * it depends on whether a recognizer (the master pacer) exists. In pure autodetect
	 * (master + a live TX pacer) it is deferred: nothing plugged -> nothing in the
	 * graph, and the node appears sized to the box the moment it is recognized (the
	 * autodetect timer below). Every other mode (slave, or pcap / no-TX master) has no
	 * recognizer, so the node is created at its startup width. */
	L->src_cfg = (struct reac_source_node_cfg){
		.loop = loop, .ring = &L->ring, .rx = &L->rx, .sample_rate = L->rx.sample_rate,
		.inst = c->inst_name,
		/* A DOOR IS NOT A MASTER. `master_role` stamps the create-time badge props of a
		 * master that is about to probe, and a refused segment probes nothing — so the
		 * door takes the slave shape, which is also what stamps reac.segment on this
		 * node and makes it the segment's one doorway (reac_segment_ident.h). */
		.master_role = (c->role == REAC_ROLE_MASTER && !c->door_only),
		/* .pacer is filled once the sink exists, below: this node publishes the
		 * graph-clock sample too, because it is the node a console actually links
		 * and therefore often the only one the graph drives. */
	};

	/* A REFUSED SEGMENT IS A DOOR AND NOTHING ELSE (0.5.1, DESIGN.md). The wire is
	 * pinned MASTER and a stagebox is mastering it, or a rival nobody can read is: two
	 * answers that contradict each other, and the daemon settles it by refusing rather
	 * than out-shouting a box. What it must NOT do is disappear — the 2026-09-09 rig
	 * proof refused correctly and published nothing, so the console had an absence to
	 * render and the operator had no remedy to read. So: one node, carrying the segment's
	 * identity and the refusal props, and no engine at all behind it — no TX, no pacer,
	 * no segment lock, and no RX feeder (hearing_serve does not start one), which is why
	 * the ports are silent rather than carrying a wire we declined. */
	if (c->door_only) {
		/* A VACANT DOOR IS NO DOOR AT ALL (operator, 2026-09-16). `door_vacant` is the
		 * tap that heard NOTHING — no master, no box, zero channels — and it published
		 * a 0-port node so the segment could be seen and its role changed back. That is
		 * the same `none / 0 in` device the empty trunk VLAN produced, arriving by the
		 * other door, and the role is settable without it now (reac-pw.conf). A REFUSAL
		 * keeps its door: it carries a width, a rival's address and a remedy — facts
		 * about a box that IS there — and a refusal nobody can see is indistinguishable
		 * from a daemon that is not running (the 2026-09-09 rig proof). Zero channels
		 * is the line between them, and it is the line the operator drew. */
		if (c->door_vacant) {
			fprintf(stderr, "reac-pw: %sTAP with nothing to serve — NOTHING is on the "
			        "graph for this segment. It is published when a master's stream is "
			        "heard on it; until then this journal is where it exists, and "
			        "reac-pw.conf is where its role is set.\n", c->tag);
			return 0;
		}
		if (reac_source_node_ensure(&L->src, &L->src_cfg,
		                            (int)c->wire_channels, NULL) != 0) {
			fprintf(stderr, "reac-pw: %sREFUSED, and the door node could not be "
			        "created — the refusal is in this journal and nowhere else\n",
			        c->tag);
			reac_rx_close(&L->rx);
			reac_ring_free(&L->ring);
			return -1;
		}
		reac_role_swap_opened(&L->role_swap, c->role);
		reac_source_node_set_role_swap(L->src, &L->role_swap);
		listener_publish_segment(L);
		fprintf(stderr, "reac-pw: %sREFUSED — publishing the door only: "
		        "reac-capture at %u ch carrying reac.master.refusal, the rival's "
		        "address and the segment name. Nothing is transmitted, nothing is "
		        "received, and the remedy is on the box's own front panel\n",
		        c->tag, c->wire_channels);
		return 0;
	}

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
		/* #75/#77: layered (reac_conf_lookup), so each is also settable in
		 * reac-pw.env and announced at start like every other knob (§1).
		 * REACPW_CLOCK_REF used to stay a bare getenv here: this cfg is forwarded
		 * into a node that OUTLIVES this block, and neither consumer copied it, so
		 * a conf-layer stack buffer would have dangled. Closed 2026-09-17:
		 * resolve_clock_ref() (below) resolves once into a STATIC buffer (the same
		 * process-lifetime guarantee getenv() gave, now through argv/conf/env too)
		 * and reac_source_node.c copies its own besides (matching
		 * reac_sink_node.c's existing n->clock_ref[64] pattern) — the last
		 * env-only knob exception in g_reac_knobs is gone. */
		char v_catchup[16];
		int clock_follow = reac_conf_flag("REACPW_CLOCK_FOLLOW", REAC_CLOCK_FOLLOW_DEFAULT);
		const char *clock_ref = resolve_clock_ref();
		int catchup_max_slots =
		    reac_conf_lookup("REACPW_CATCHUP_MAX_SLOTS", NULL, NULL, v_catchup,
		                     sizeof v_catchup) != REAC_CONF_NONE ? atoi(v_catchup) : 0;
		int rate_match_off = reac_conf_flag("REACPW_RATE_MATCH", 0) ? 0 : -1;
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
		                              /* #75: the discipline is the DEFAULT since 0.5.0.
		                               * REACPW_CLOCK_FOLLOW=0 opts out and gets
		                               * the free-run, which is then REPORTED rather than
		                               * silent (reac_sink_node.h carries the ruling). */
		                              .clock_follow = clock_follow,
		                              /* --rate / a conf-file rate is an ASSERTION; only the
		                               * built-in best-drivable pick is the convention. */
		                              .rate_asserted = c->rate_layer != REAC_CONF_BUILTIN
		                                            && c->rate_layer != REAC_CONF_NONE,
		                              /* #77: unset -> nothing is designated and the
		                               * name heuristic alone grades the reference. */
		                              .clock_ref = clock_ref,
		                              /* Slot-debt budget. Unset -> the measured
		                               * default; see reac_pacer.h. */
		                              .catchup_max_slots = catchup_max_slots,
		                              /* RATE MATCHING SHIPS OFF. OPT IN WITH
		                               * REACPW_RATE_MATCH=1. See reac_pacer.h's
		                               * cfg.rate_match_off for the full measurement —
		                               * unchanged by this refactor. */
		                              .rate_match_off = rate_match_off };
		/* CLAIM THE SEGMENT BEFORE THE FIRST FRAME. Driving is what takes the
		 * lock; RX above has been running unlocked, which is correct — observing a
		 * segment is a copy and must stay safe beside somebody else's master. */
		/* AND THE WIRE HAS TO HAVE ROOM FOR IT. A REAC master's downstream is a
		 * fixed 1492 B broadcast at sample_rate/12 pps — ~97 Mbit/s at 96 kHz — and
		 * a 100 Mbit/s port carries exactly one. On 2026-09-16 this port carried
		 * FOUR declared masters (the untagged segment and VLANs 11, 12 and 13, three
		 * of them with no box on them at all): 387 Mbit/s offered, the link pinned at
		 * 99.6 Mbit/s, and the port's etf qdisc discarding 23 784 pkt/s as overlimit
		 * — 75% of EVERY segment's frames, evenly. Every health sign stayed green:
		 * the nodes were up, each pacer counted its own frames, and the drops sat on
		 * a counter nobody reads. What an S-1608 in slave mode saw was a master
		 * stream at a quarter of its packet rate and invitations thinned the same
		 * way, so it never locked and rx stayed 0 for hours.
		 *
		 * Refusing is the only honest answer: a quarter of a stream is not a degraded
		 * master, it is a silent one, and the operator can move a segment to another
		 * port or drop a rate the moment they are told which one did not fit. */
		unsigned link_mbit = link_speed_mbit(c->tx_if);
		uint64_t want_kbit = reac_link_cost_kbit(reac_link_master_pps(L->rx.sample_rate),
		                                         REAC_FRAME_BYTES);
		uint64_t used_kbit = link_used_kbit(L);
		/* AND IF IT DOES NOT FIT, ASK WHETHER WHAT IS HOLDING THE PORT IS CARRYING
		 * ANYTHING (#107, auto-role amendment 2026-09-20). Only here, and only when
		 * this wire has REAC gear on it: a yield costs a neighbour its engine, so it
		 * is taken exactly when the alternative is this segment being refused, and
		 * never as a standing preference. On a port with room — a 1 Gbit trunk, or
		 * any link whose speed cannot be read — this is not reached at all and no
		 * neighbour is ever touched. */
		if (!reac_link_budget_fits(link_mbit, used_kbit, want_kbit) && c->wire_heard &&
		    link_budget_yield(c->rxcfg.source ? c->rxcfg.source : c->tx_if,
		                      c->tx_if, monotonic_ns()) > 0)
			used_kbit = link_used_kbit(L);   /* re-read: never assume the yield freed it */
		if (!reac_link_budget_fits(link_mbit, used_kbit, want_kbit)) {
			char port[IFNAMSIZ];
			link_port_of(c->tx_if, port, sizeof port);
			/* AND WHO HAS IT (#107). A refusal that names a number and not a
			 * holder made the operator dump the roster to find out that the
			 * segment keeping the box off the wire was an empty VLAN. */
			char holders[512];
			link_budget_holders(L, holders, sizeof holders);
			reac_code_emit(stderr, "reac-pw", RC_E_LINK_BUDGET,
			    "%sREFUSING to master '%s' — it does not FIT on %s.\n"
			    "         This master costs %llu kbit/s (%u pps x %d B at %u Hz);\n"
			    "         %llu kbit/s of %s's %u Mbit/s is already committed to other\n"
			    "         REAC masters. Transmitting anyway does not share the wire,\n"
			    "         it fills it: the port's qdisc then discards frames from\n"
			    "         EVERY segment on it and no box can sync to any of them.\n"
			    "         Held by: %s.\n"
			    "         Move this segment to another port, or lower a rate.\n",
			    c->tag, c->tx_if, port,
			    (unsigned long long)want_kbit, reac_link_master_pps(L->rx.sample_rate),
			    REAC_FRAME_BYTES, L->rx.sample_rate,
			    (unsigned long long)used_kbit, port, link_mbit, holders);
			reac_rx_close(&L->rx);
			reac_ring_free(&L->ring);
			reac_ring_free(&L->tx_ring);
			return -1;
		}

		int claimed = reac_seglock_claim(&L->seglock, c->tx_if);
		if (claimed == -1) {
			char holder[160];
			describe_seglock_holder(L->seglock.name, holder, sizeof holder);
			reac_code_emit(stderr, "reac-pw", RC_E_SEGMENT_HELD,
			    "%sREFUSING to master '%s' — another process already holds\n"
			    "         that segment (%s)%s%s. Two masters on one segment is the\n"
			    "         fault this lock exists to make impossible; it has cost an\n"
			    "         evening once and corrupted a live measurement once.\n"
			    "         Nothing is taken over automatically: stop the holder, or\n"
			    "         drive a different segment. Who holds it:\n"
			    "           grep %s /proc/net/unix\n",
			    c->tag, c->tx_if, L->seglock.name,
			    holder[0] ? ", held by " : "", holder, L->seglock.name);
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
		if (c->n_headamps) {
			/* Say which of the two policies is actually running. An operator
			 * reading "armed" cannot otherwise tell whether the wire will refresh
			 * these cells or assert them once and go quiet, and that is the whole
			 * difference between the console owning the box's pins and the box
			 * panel owning them. */
			char refresh[64];
			if (REAC_HEADAMP_RESWEEP_SECONDS > 0)
				snprintf(refresh, sizeof refresh, "refreshed every %d s",
				         REAC_HEADAMP_RESWEEP_SECONDS);
			else
				snprintf(refresh, sizeof refresh,
				         "no periodic refresh (re-assert disabled)");
			fprintf(stderr, "reac-pw: %shead-amp DMX send armed — %d cell(s), "
			        "asserted once established, %s "
			        "(RIG-GATED: verify 48V at the XLR pins)\n",
			        c->tag, c->n_headamps, refresh);
		}
	} else if (c->tx_if && c->role == REAC_ROLE_SLAVE) {
		/* The slave returns its OWN input channels (a box width) upstream. The PCM
		 * for them would come from a reac:return sink; for now the ring is the
		 * carrier and the slave emits silent/own-input FILLER until that sink is
		 * linked. The engine learns the master MAC from the wire — never set here. */
		uint8_t box_mac[6];
		if (c->src_mac_set) {
			memcpy(box_mac, c->src_mac, 6);
		} else if ((c->join_box_master || c->box_model) &&
		           reac_mac_default_src(c->tx_if, box_mac) == 0) {
			/* AND THE BOX ROLE IS THE OTHER CALLER (2026-09-17). Every box in this
			 * corpus announces from a Roland OUI, and a master's own recognition
			 * keys on it: measured in tests/box-declares-its-row.sh, our master
			 * counted `rx_box_frames=0` against a box-role daemon flooding 628 B
			 * frames from this NIC's 56:b1:… address, and logged `model=unknown`.
			 * A box we cannot be recognised as is not an emulated box. */
			/* THE ONE WIRE WHERE THE ADDRESS IS NOT VERBATIM (0.5.6, reac_mac.h).
			 * Every box this rig has ever granted announced from a Roland OUI; the
			 * S-0808 was sent four correct cold-connect bursts from this NIC's own
			 * 00:14:5c:… and echoed nothing, lamp blinking. Roland's OUI over this
			 * NIC's host part, so it cannot collide with a real box and a capture
			 * still says which machine spoke. --src-mac overrides it. */
			uint8_t nic[6];
			memcpy(nic, box_mac, 6);
			reac_mac_roland_standin(nic, box_mac);
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
		        /* THE ANNOTATION READS THE SAME CONDITION THE ADDRESS DID (2026-09-17).
		         * The Roland standin is taken for the box role too, so keying this on
		         * join_box_master alone made a box-role listener describe a Roland-OUI
		         * address as its own NIC's -- and this line is what a capture is read
		         * against. */
		        c->src_mac_set ? " (--src-mac override)"
		                       : (c->join_box_master || c->box_model)
		                           ? " (Roland OUI + this NIC's host part, the one wire that"
		                             " is not verbatim; --src-mac overrides)"
		                           : " (this NIC's own address; --src-mac overrides)");
		reac_ring_init(&L->tx_ring, REAC_MAX_CHANNELS, (uint32_t)(L->rx.sample_rate / 4));
		L->tx_ring_init = 1;
		/* WHAT WE SEND, AND WHAT WE DECLARE, ARE TWO NUMBERS ON A BOX-MASTER WIRE
		 * (0.5.6, operator ruling: "mixer always sends 40ch, boxes send their width
		 * only"). To a DESK we are the box: one width, our own, declared and sent.
		 * To a STAGEBOX ON M we are the MIXER — every audio frame is the 1492 B
		 * 40-slot downstream with the box's outputs in their slots, so the width
		 * passed here is the BOX'S OUTPUT count and it names slots inside that frame,
		 * never the frame's own geometry (reac_slave.c's bm_downstream). What we
		 * DECLARE stays ours: the S-1608 sent 8 slots of audio to that same chassis
		 * and announced its own 16-input self. */
		/* THE BOX'S OUTPUT COUNT, NOT ITS INPUT WIDTH (0.5.6-9). What we send a box
		 * master feeds its OUTPUTS, so the slots are its out count — and both real
		 * captures agree: an S-1608 slave sent 8 slots to an 8-out master, and an
		 * S-0808 slave sent 8 slots to a 16-in/8-out master. (Both boxes are 8-out,
		 * so "the master's outputs" and "the slave's outputs" are not yet told apart
		 * by any capture; the master's is what the frames feed and is what is used.)
		 * `wire_channels` is the width the box BROADCASTS, which is its INPUT count —
		 * using it sized an S-1608's playback door to 16 where the box has 8. */
		const struct reac_box_model *bm_up = c->join_box_master
			? reac_box_master_model(c->wire_channels) : NULL;
		int up_ch = c->join_box_master
			? (bm_up ? bm_up->out_ch : (int)c->wire_channels)
			: c->box_channels;
		/* The rig experiment, no rebuild between runs: REACPW_BOX_MASTER_FRAME=box
		 * imitates the S-1608 exactly (340 B at the master's width, unicast);
		 * anything else is the ruling's 40-ch mixer frame. Read here because main
		 * owns the environment. Layered (reac_conf_lookup) so each can also be set
		 * in reac-pw.env, and announced at start like every other knob (§1). */
		char v_frame[16], v_burst[16], v_fill[16], v_presil[16];
		int box_master_frame_box =
		    reac_conf_lookup("REACPW_BOX_MASTER_FRAME", NULL, NULL, v_frame, sizeof v_frame)
		        != REAC_CONF_NONE && strcmp(v_frame, "box") == 0;
		int box_master_burst_chanmap =
		    reac_conf_lookup("REACPW_BOX_MASTER_BURST", NULL, NULL, v_burst, sizeof v_burst)
		        != REAC_CONF_NONE && strcmp(v_burst, "chanmap") == 0;
		int box_master_fill_noise =
		    reac_conf_lookup("REACPW_BOX_MASTER_FILL", NULL, NULL, v_fill, sizeof v_fill)
		        != REAC_CONF_NONE && strcmp(v_fill, "noise") == 0;
		int box_master_presilence_ms =
		    reac_conf_lookup("REACPW_BOX_MASTER_PRESILENCE_MS", NULL, NULL, v_presil,
		                     sizeof v_presil) != REAC_CONF_NONE ? atoi(v_presil) : 0;
		struct reac_slave_cfg slcfg = { .ifname = c->tx_if,
		                                /* THE ROW WE DECLARE (box role); NULL on every
		                                 * other slave, where the width keys the
		                                 * captured matrix as it always has. */
		                                .model = c->box_model,
		                                .box_channels = up_ch,
		                                .sample_rate = L->rx.sample_rate,
		                                .src_mac = box_mac,
		                                .tag = c->tag,
		                                .box_master = c->join_box_master,
		                                .box_master_frame_box = box_master_frame_box,
		                                .box_master_burst_chanmap = box_master_burst_chanmap,
		                                .box_master_fill_noise = box_master_fill_noise,
		                                .box_master_presilence_ms = box_master_presilence_ms };
		if (reac_slave_open(&L->slave, &slcfg, &L->tx_ring) == 0) {
			L->slave_open = 1;
			if (reac_slave_start(&L->slave) == 0) {
				reac_slave_set_phy_up(&L->slave, 1);  /* PHY up: begin the establishment */
				if (c->join_box_master)
					fprintf(stderr, "reac-pw: %sSLAVE role on a BOX MASTER — "
					        "enrolling with it the way a stagebox does: %d-ch "
					        "broadcast flood at ITS width, then unicast "
					        "config-announce, then the cold-connect burst, then "
					        "its outputs from reac-playback at the wire rate\n",
					        c->tag, up_ch);
				else if (c->box_model)
					/* SAID AS THE ROSTER SAYS IT. This is the same segment the
					 * conf pinned `box` and the roster publishes as `box`; it
					 * used to read `SLAVE role` here, which is the engine's
					 * name for the wire end and not the operator's word. */
					fprintf(stderr, "reac-pw: %srole = box (%d-ch upstream "
					        "return) — answering the mixer as the row we declare, "
					        "locked to its cadence\n", c->tag, up_ch);
				else
					fprintf(stderr, "reac-pw: %srole = slave (%d-ch upstream return) — "
					        "responding to an external master, locked to its cadence\n",
					        c->tag, up_ch);
				/* THE BOX MASTER'S OUTPUTS ARE ROUTABLE FROM HERE (0.5.6). The
				 * upstream we unicast to it IS what reaches those outputs, so the
				 * segment gets the same reac-playback node an operator patches in
				 * the master role — ports, gain staging, identity — with the slave
				 * engine's ring as its carrier instead of a pacer. Sized to the
				 * width the wire declared, and labelled with the model that width
				 * identified where it identifies one (0.5.2). */
				/* AND THE SINK IS WHAT WE SEND: the box's INPUTS, which arrive on
				 * the mixer's input channels. The same node the box-master path
				 * publishes over the same ring — one mechanism, two callers. */
				if (c->join_box_master || c->box_model) {
					const struct reac_box_model *bm = c->box_model ? c->box_model
					                                               : bm_up;
					/* ONE NUMBER, SPELLED ONCE. The width of what we send was
					 * written out three times in this block and REPORTED as a
					 * fourth, different one (`up_ch`) when the sizing failed —
					 * a message about a size nobody tried. */
					const int sink_ch = c->box_model ? c->box_model->in_ch : up_ch;
					struct reac_sink_cfg ucfg = {
						.ifname = c->tx_if,
						.channels = sink_ch,
						.sample_rate = L->rx.sample_rate,
						.src_mac = box_mac,
						.console_field = c->mixer->console_field,
						.inst = c->inst_name,
						.label = bm ? bm->display : NULL,
						.rate_match_off = -1,
						.upstream_ring = &L->tx_ring };
					L->sink = reac_sink_node_new(loop, &L->tx_ring, &ucfg);
					/* ONE TEXT FOR BOTH CALLERS. What reac-playback carries is
					 * the upstream we send — the box master's outputs on a join,
					 * the mixer's inputs under role = box — and naming only the
					 * first misdescribed the second. What both need said is the
					 * width that was tried and that nothing can be routed. */
					if (!L->sink)
						reac_code_emit(stderr, "reac-pw", RC_E_SIZING,
						        "%sthe %d channels we send "
						        "upstream have no reac-playback node — what "
						        "arrives still arrives, but nothing can be "
						        "routed out\n", c->tag, sink_ch);
					else if (reac_sink_node_ensure(L->sink, sink_ch,
					                               bm ? bm->display : NULL) != 0)
						reac_code_emit(stderr, "reac-pw", RC_E_SIZING,
						        "%scould not size reac-playback "
						        "to the %d channels we send upstream\n",
						        c->tag, sink_ch);
				}
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
	if (L->sink) {
		reac_sink_node_set_rate_source(L->sink, &L->rx);
		/* Before any source node is built (the deferred autodetect path builds them
		 * from this same cfg), so every rebuild carries the clock door. */
		L->src_cfg.pacer = reac_sink_node_pacer(L->sink);
		L->src_cfg.clock_ref = resolve_clock_ref();
	}
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
		L->adc.ifname = L->cfg.tx_if;       /* the device the wake ladder may bounce */
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
		L->adc.pinned = (c->pin_model != NULL);
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
				/* A FAILED OPEN TAKES ITS OWN NODES WITH IT (#108 §a.3). Half a pair
				 * was built by the calls above, and hearing_serve MEMSETS a listener
				 * whose open refused — so returning here without this left a node on
				 * the graph with no pointer to it anywhere in the process. */
				goto fail_with_nodes;
			}
			fprintf(stderr, "reac-pw: %sMASTER pinned --box %s — reac-capture %d ch / "
			        "reac-playback %d ch labelled '%s', present from boot. The pin names "
			        "and sizes the ports; the WIRE still decides what is enrolled, and "
			        "outranks the pin if a different box declares itself.\n",
			        c->tag, c->pin_model->token, c->pin_model->in_ch, c->pin_model->out_ch,
			        c->pin_label);
		} else {
			/* NO RECOGNISED BOX, NO NODE (operator, 2026-09-16; the autodetect spec's
			 * later amendment). This used to build reac-playback at ZERO ports so the
			 * segment had an identity and a role door before anything enrolled —
			 * Q5 option C. What the desk got from it was a device reading `none /
			 * 0 in` for an empty trunk VLAN, a row for a thing that is not there. The
			 * requirement that ruling served is not withdrawn: a role is settable
			 * before anything enrols through reac-pw.conf, which needs no node at all.
			 *
			 * THE COST IS REAL AND IS NAMED RATHER THAN HIDDEN: openmixer's
			 * /reac/segment roster is a GRAPH SCAN of reac.segment props, so until the
			 * console reads the daemon's roster from somewhere that is not a node, an
			 * empty segment is visible HERE and nowhere else. Which is what this line
			 * is for. */
			fprintf(stderr, "reac-pw: %sMASTER autodetect — PROBING, and NOTHING is on "
			        "the graph for this segment: reac-capture / reac-playback are "
			        "created when a box is recognized on this wire, and removed again "
			        "when it leaves. A segment with no box is a line in this journal "
			        "and not a device.\n", c->tag);
		}
	} else {
		/* No recognizer (slave, or pcap / no-TX master): expose the source now, at
		 * the full 40-slot fabric. With no recognizer there is nothing that could
		 * honestly narrow it to a box, and nothing may pretend otherwise.
		 *
		 * EXCEPT WHEN THE WIRE ITSELF DECLARED A WIDTH (0.5.1). A box mastering the
		 * segment broadcasts its own geometry, and that width is evidence, not a
		 * guess — an 8-channel box gets an 8-port capture node rather than a 40-slot
		 * fabric with 32 rows of silence in it. */
		/* A BOX ROLE'S CAPTURE IS WHAT THE MIXER SENDS US — the row's OUTPUT count,
		 * because the ports are named from the BOX's side and the graph's from ours
		 * (2026-09-17 spec §5). It is known before any mixer appears, which is the
		 * point: a stagebox that only exists once a desk is powered is not a
		 * stagebox. */
		int width = c->box_model ? c->box_model->out_ch
		                         : (c->join_box_master ? (int)c->wire_channels : 0);
		/* AND IT NAMES THE BOX, as the master path's capture node does (0.5.6-9). The
		 * identity keys were published either way, but a console reads the node's
		 * DESCRIPTION for the operator-facing name, so a joined box read the generic
		 * "REAC 16ch capture" where a served one reads "S-1608 (16 in / 8 out)". The
		 * label comes from the row the broadcast width matched, and is absent where no
		 * row matches — the same rule the identity keys already follow. */
		const struct reac_box_model *bm_cap = c->box_model
			? c->box_model
			: (c->join_box_master ? reac_box_master_model(c->wire_channels) : NULL);
		if (reac_source_node_ensure(&L->src, &L->src_cfg, width,
		                            bm_cap ? bm_cap->display : NULL) != 0) {
			fprintf(stderr, "reac-pw: %sfailed to create reac:capture node\n", c->tag);
			/* The box-master join built a reac-playback above (one mechanism, two
			 * callers) and it is on the graph right now; the same #108 §a.3 rule. */
			goto fail_with_nodes;
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

	/* THE ONE EXIT THAT HAS ALREADY BUILT SOMETHING THE GRAPH CAN SEE (#108 §a.3). The
	 * ring, the socket and the seglock are cleaned up at each refusal above because each
	 * knows what it opened; the NODES were the one resource nobody took back, and the
	 * caller cannot take them back either — hearing_serve memsets a listener whose open
	 * refused, and main()'s array entry is simply left. The code makes it searchable:
	 * reaching this at all means a node lived, however briefly, for a segment that never
	 * came up. */
fail_with_nodes:
	reac_code_emit(stderr, "reac-pw", RC_E_ORPHAN_PAIR,
	    "%sthe open refused AFTER putting node(s) on the graph — removing them, because "
	    "a segment that did not come up must leave nothing behind for a console to read\n",
	    c->tag);
	listener_drop_nodes(L, loop);
	return -1;
}

/* Tear down one segment, mirroring main()'s single-instance shutdown block.
 * Safe on a partially-opened L (every destroy/close/free below is documented
 * NULL/unheld-safe), so it doubles as listener_open()'s own failure cleanup. */
static void listener_close(struct listener *L, struct pw_loop *loop)
{
	/* ONLY A FEEDER THAT WAS STARTED IS JOINED. reac_rx_stop joins the thread
	 * unconditionally, and joining a pthread_t that was never created is a
	 * dereference of nothing — reachable from the failed-start path since it was
	 * written, and a NORMAL path since 0.5.1's door-only segment, which deliberately
	 * runs no feeder at all. */
	if (L->cfg.tap) {
		listener_close_tap(L);
		/* A VACANT TAP FELL THROUGH INTO THE DOOR and may hold `src` as well as
		 * `tap_src[]`; one teardown owns every node either way. */
		listener_drop_nodes(L, loop);
		reac_role_swap_closed(&L->role_swap);
		return;
	}
	if (L->rx_started)
		reac_rx_stop(&L->rx);
	/* ONE TEARDOWN, AND IT NULLS WHAT IT FREES (#108 §a.2). This block used to destroy
	 * `src` and `sink` and leave both pointers DANGLING at their freed structs, which is
	 * why the open's "did this listener still hold a pair?" question could not be asked
	 * at all: it had no way to tell a live pair from a freed one. The timer is stopped
	 * first, as it always was — it rebuilds the very nodes it is about to lose. */
	listener_drop_nodes(L, loop);
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

struct hearing;

struct sniffer {
	char name[IFNAMSIZ];        /* "" = free slot */
	struct reac_capture cap;
	struct spa_source *io;
	uint8_t mac[6];             /* the NIC's own address: our echo, if any, is not a sighting */
	unsigned long frames;       /* 0x8819 frames read, whether or not they classified */
	/* WHICH END OF THE PAIRING THIS SEGMENT WILL TAKE (reac_hunt.h). The sniffer is
	 * where the evidence arrives, so the hunt lives here and dies with it: a segment
	 * that is served, or a link that goes away, gets a fresh hunt next time. */
	struct reac_hunt hunt;
	int undecided_said;         /* the "heard, nothing decides it yet" line, said once */
	/* THE MASTERLESS OBSERVATION (reac_knock.h). A wire nobody pinned that carries not
	 * one frame for REAC_KNOCK_LISTEN_NS has no master on it — a master fills every
	 * audio slot — and is then DRIVEN, because a cold box in slave mode never speaks
	 * first and a lone announce does not wake one (measured on the rig with 0.5.0-3).
	 * A PINNED interface does not need it: it is already driving. */
	struct reac_knock knock;
	int watch_silence;          /* 0 = pinned, so the observation does not apply */
	/* WHEN THIS SNIFFER LAST HEARD A REAC FRAME (0 = never). The tap-authority wait in
	 * hearing_hunt is measured from it, so that "something was heard here" can expire
	 * the way every other fact about a wire does — reac_tapwait.h. */
	uint64_t last_heard_ns;
	/* THIS SEGMENT IS PINNED `tap` (segment_tap_pin). Kept here because the hunt's
	 * clock needs it for two things it cannot get from reac_hunt: a tap is SERVED ON
	 * LINK like any other pin (the door ruling), and a tap's wire is NEVER driven —
	 * the masterless licence below must not be armed on a mirror port. */
	int tap_pinned;
	int tap_served;             /* the serve was queued once; not queued again */
	/* EVERY WIRE WE TOOK AND NOBODY PINNED KEEPS ITS SNIFFER (0.5.4, reac_watch.h).
	 * The wire is ours only while nobody else claims it, so it goes on being
	 * classified and hearing_yield acts on what it hears: a desk that turns up second
	 * is yielded to, and one that goes home hands the segment back. Until 0.5.4 this
	 * was set only where the wire had been taken on proven SILENCE, and a wire won
	 * because a box was heard on it kept nothing — the venue case, exactly. */
	int watched;
	/* HOW the wire was won, kept for the sentence the yield prints and nothing else. */
	int driven_on_silence;
	/* THIS SEGMENT GAVE ITS PORT'S LINK BUDGET UP (#107, auto-role amendment
	 * 2026-09-20), and it does not take it back on the same evidence that won it the
	 * first time. The silence licence is an argument about an empty wire and is just
	 * as true five seconds later, so without this latch two empty segments on one
	 * 100 Mbit port would hand the budget to each other for ever. It is cleared by
	 * the segment's OWN evidence — reac_hunt_heard_anything, something actually heard
	 * here — and by nothing else, not by a timer and not by the neighbour's state. */
	int budget_yielded;
	int budget_yield_said;      /* "still listening, yielded" — said once, never a spinner */
};

/* THE TOPOLOGY TAP, one per physical parent with carrier. ETH_P_ALL, BPF-filtered to
 * 0x8819, PACKET_AUXDATA on, never transmitting — the only socket that can tell a tagged
 * frame from an untagged one (reac_topo.h has the measurement). It FEEDS the table and
 * nothing else: every netdev the table asks for is made or removed from the 200 ms poll,
 * so no source is ever destroyed from inside its own callback. */
struct topo_tap {
	char parent[IFNAMSIZ];     /* "" = free slot */
	struct reac_topo_tap tap;
	struct spa_source *io;
	struct hearing *h;
	int said_trunk;            /* "this parent is a trunk, not a segment", said once */
	/* WHAT THE TAP IS BOUND TO (#102). Only a frame the kernel says arrived on THIS
	 * ifindex is evidence about this parent; the reader above has the measurement. */
	unsigned ifindex;
	int said_foreign;          /* "a frame from elsewhere reached this tap", said once */
	uint8_t last_tagged_src[6];/* who sent the last tag counted here, for the log line */
	/* WHICH VIDS HAVE CARRIED TAGGED **REAC** HERE — evidence about frames, kept for the
	 * journal, beside `last_tagged_src` and for the same reason (#102). Since libreac
	 * 1.5.0 a VID can be heard from a tag on ANY ethertype (that is how a cold VLAN is
	 * found), so "tagged REAC heard — vid N" is no longer true of every ENSURE, and a
	 * line that said it anyway would be the #102 report with invented evidence. NOT a
	 * segment list and never read as one: the segments are the library's table.
	 * `reac_vids_n` is the live count, capped at the library's own per-parent bound. */
	uint16_t reac_vids[REAC_TOPO_MAX_VLANS];
	int reac_vids_n;
	/* WHAT THIS CABLE CARRIES **NOW** (#98). The topology table is deliberately kept
	 * across a link bounce — deleting it would destroy the segments the ifscan hold
	 * exists to preserve — but the TRUNK VERDICT is a claim about the present, and
	 * across a re-patch it was not. Measured on the rig 2026-09-13: a USB NIC spent an
	 * hour on a switch mirror, was classified a trunk, was then moved onto an S-4000M
	 * directly, and its pinned master answered `untagged REAC on a trunk's native VLAN
	 * is not served` — the box got no master until the daemon was restarted. So the tap
	 * stamps its own link-up and remembers whether a tag has been heard SINCE it; a
	 * verdict with no tag behind it since this cable came up is stale and does not
	 * refuse anything. Both are reset by topo_watch_iface's memset at every link-up.
	 *
	 * AND IT IS A ROLLING WINDOW, NOT A LATCH (#102). `tagged_since_linkup` was a
	 * sticky flag, so ONE frame — on the rig, one of our own, misattributed by the
	 * unbound tap the reader above documents — made the verdict permanent and #98's
	 * re-proof could never run on the start path. A trunk that is still a trunk puts
	 * thousands of tagged frames a second on the wire, so the honest question is when
	 * the LAST tag was heard, not whether one ever was. */
	uint64_t linkup_ns;
	uint64_t last_tagged_ns;   /* 0 = not one tag since this link came up */
	int said_stale;            /* "no tag since link-up, serving it untagged", said once */
};

/* HOW LONG AFTER LINK-UP A TRUNK MUST PROVE ITSELF AGAIN (#98). Read off the cadence,
 * like every other window here: anything mastering a VLAN on this parent transmits at
 * the wire cadence — thousands of tagged frames a second — so a trunk that is still a
 * trunk is heard almost immediately. Three master announce cadences (REAC_HUNT_WINDOW_NS)
 * is the same bar the hunt uses for "nothing decides this wire", and it survives a PHY
 * renegotiation and a switch port coming out of listening state. */
#define REACPW_TRUNK_RECLASSIFY_NS REAC_HUNT_WINDOW_NS

/* One declared segment and what the daemon did about it. `minted` is the whole exit
 * contract: what we created we remove, what we adopted we leave. */
struct declared_seg {
	struct reac_declared_vlan d;
	char name[IFNAMSIZ];        /* `<parent>.<vid>`, resolved once */
	int  present;               /* the netdev is there and up */
	int  minted;                /* WE created it, so the exit takes it away */
	int  said_waiting;          /* the "parent is not here yet" line is printed once */
	int  said_failed;           /* and so is the refusal, until it next succeeds */
};

struct hearing {
	int enabled;
	struct reac_ifscan scan;
	struct reac_topo topo;
	struct topo_tap tap[REAC_TOPO_MAX_PARENTS];
	struct spa_source *nl_io;
	struct sniffer sniff[REAC_IFSCAN_MAX];
	struct listener *listeners;
	int n_slots;
	struct pw_loop *loop;
	int forced_rate;            /* a whole-invocation --rate, applied to every heard segment */
	unsigned long served, dropped;
	/* THE DECLARED SEGMENTS, which do not wait to be heard (reac_declared_vlan.h). A
	 * separate ledger from reac_topo's on purpose: that one releases a VID silent for
	 * 30 s, and silence is a declared segment's STARTING condition on a cold rig. */
	struct declared_seg decl[REAC_DECLARED_VLAN_MAX];
	int n_decl;
	int decl_full;              /* the scan hit REAC_DECLARED_VLAN_MAX; reported, not hidden */
	int decl_recheck;           /* an RTM_NEWLINK arrived: look at the parents again */
};

static struct hearing g_hear;


/* Counted from the RUNNING engines, never from the roster: a segment that is configured
 * and not transmitting costs the wire nothing, and a budget read off intentions would
 * refuse a master because of one that never opened. */
static uint64_t link_used_kbit(const struct listener *self)
{
	char mine[IFNAMSIZ];
	link_port_of(self->cfg.tx_if, mine, sizeof mine);
	uint64_t used = 0;
	for (int i = 0; i < g_hear.n_slots; i++) {
		const struct listener *L = &g_hear.listeners[i];
		if (L == self || !L->opened || !L->sink)
			continue;
		char theirs[IFNAMSIZ];
		link_port_of(L->cfg.tx_if, theirs, sizeof theirs);
		if (strcmp(mine, theirs) != 0)
			continue;
		used += reac_link_cost_kbit(reac_link_master_pps(L->rx.sample_rate),
		                            REAC_FRAME_BYTES);
	}
	return used;
}

/* IS THIS BUDGET HOLDER CARRYING ANYTHING AT ALL? (#107, auto-role amendment 2026-09-20.)
 *
 * NO NEW CLASSIFIER: these are the same three facts `port_siblings_served` below already
 * trusts to answer "would a bounce cost anybody anything", asked of one listener instead
 * of a port. A master proves it is carrying something by being past PROBING (granting or
 * established), by having RECOGNISED a box, or by the segment's own RX latch saying the
 * frames are still arriving. None of the three, and the engine on that wire is flooding
 * 97 Mbit/s of downstream at nobody — which is what held the 100 Mbit trunk of #107.
 *
 * A TAP AND A DOOR ARE NOT HOLDERS and never reach here: neither opens a sink, and
 * `link_used_kbit` counts sinks. */
static int listener_budget_empty(const struct listener *L)
{
	if (!L->opened || !L->sink || L->cfg.tap)
		return 0;
	if (L->heard.heard)
		return 0;
	if (reac_sink_node_past_probing(L->sink))
		return 0;
	if (reac_sink_node_recognized_box(L->sink))
		return 0;
	return 1;
}

static void link_budget_holders(const struct listener *self, char *out, size_t cap)
{
	char mine[IFNAMSIZ];
	link_port_of(self->cfg.tx_if, mine, sizeof mine);
	size_t n = 0;
	out[0] = '\0';
	for (int i = 0; i < g_hear.n_slots; i++) {
		const struct listener *L = &g_hear.listeners[i];
		if (L == self || !L->opened || !L->sink)
			continue;
		char theirs[IFNAMSIZ];
		link_port_of(L->cfg.tx_if, theirs, sizeof theirs);
		if (strcmp(mine, theirs) != 0)
			continue;
		/* WHICH of the three facts makes it a holder, in the holder's own order of
		 * evidence — not one word for all of them. A master that has RECOGNISED a
		 * box but has not granted yet is neither empty nor established, and calling
		 * it either would be a sentence that does not match the act. */
		const char *what = L->heard.heard ? "carrying frames"
		                 : reac_sink_node_past_probing(L->sink) ? "established"
		                 : reac_sink_node_recognized_box(L->sink) ? "probing, box heard"
		                 : L->cfg.role_pinned ? "probing, no box, pinned by "
		                                        REAC_SEGCONF_FILE
		                                      : "probing, no box";
		if (n + 1 >= cap)
			break;           /* full: the list is truncated, never overrun */
		int w = snprintf(out + n, cap - n, "%s%s (%s)",
		                 n ? ", " : "", L->cfg.tx_if, what);
		if (w < 0)
			break;
		n += (size_t)w > cap - n ? cap - n : (size_t)w;
	}
	if (!out[0])
		/* NOT "nobody": the budget did not fit and no holder of OURS is on this
		 * port, so the commitment is somebody else's — a second daemon, or a
		 * declared rate this port cannot carry on its own. Saying "nobody" over a
		 * refusal would read as a bug in the admission. */
		snprintf(out, cap, "no master of this daemon's — the port's rate is the limit");
}

/* SAME PHYSICAL PORT, AND ACTUALLY CARRYING SOMETHING. `link_port_of` strips the VLAN
 * suffix, so a parent and its `.11`/`.12`/`.13` children all answer to one port — and a
 * bounce of the parent takes every one of them down with it.
 *
 * WHAT COUNTS IS THE TRAFFIC, NOT THE ROSTER, and the difference is the whole usefulness of
 * the remedy: the desk this defect was found on serves its master on a parent with three
 * declared VLAN children, all of them VACANT DOORS that have never heard a frame. Counting
 * those as siblings would refuse every edge for ever on exactly the machine that needs one.
 * A segment that FOLLOWS proves it by `heard` (the RX latch main.c steps on its own 200 ms
 * poll); a segment that MASTERS proves it by being past PROBING. Nothing else is evidence
 * that a bounce would cost anybody anything. */
static int port_siblings_served(const char *ifname)
{
	if (!ifname || !ifname[0])
		return 0;
	char mine[IFNAMSIZ];
	link_port_of(ifname, mine, sizeof mine);
	int n = 0;
	for (int i = 0; i < g_hear.n_slots; i++) {
		const struct listener *L = &g_hear.listeners[i];
		if (!L->opened || !L->cfg.tx_if)
			continue;
		if (strcmp(L->cfg.tx_if, ifname) == 0)
			continue;                /* ourselves */
		char theirs[IFNAMSIZ];
		link_port_of(L->cfg.tx_if, theirs, sizeof theirs);
		if (strcmp(mine, theirs) != 0)
			continue;
		if (L->heard.heard) {
			n++;                     /* a tap or a slave with frames arriving */
			continue;
		}
		if (reac_sink_node_past_probing(L->sink))
			n++;                     /* another master, granting or established */
	}
	return n;
}

static uint64_t monotonic_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* IS THIS SEGMENT SWITCHED OFF? Said ONCE per segment, because the answer is asked on
 * every link event and a line per event would be a chattering port's own denial of
 * service. Never silent the first time: an interface that vanishes from the journal for
 * no stated reason is exactly the shape this spec exists to remove. */
static int segment_ignored(struct hearing *h, const char *name)
{
	(void)h;
	reac_segconf_refresh(&g_segconf);
	/* THE DECISION IS THE MODULE'S, and only the "have we said it" flag is ours. This
	 * walked g_segconf itself once, which is a second reader of one fact: sabotaging
	 * reac_segconf_ignored left this arm of segments-autodetect.sh GREEN, so the
	 * assertion was decoration until the two were joined (2026-09-16). */
	if (!reac_segconf_ignored(&g_segconf, name))
		return 0;
	struct reac_segconf_seg *s = NULL;
	for (int i = 0; i < g_segconf.n; i++)
		if (strcmp(g_segconf.seg[i].name, name) == 0) {
			s = &g_segconf.seg[i];
			break;
		}
	if (!s)
		return 1;
	if (!s->said_ignored) {
		s->said_ignored = 1;
		fprintf(stderr, "reac-pw: [%s] IGNORED by %s [segment %s] ignore — not sniffed, "
		        "not served, not minted; its netdev is left exactly as found\n",
		        name, REAC_SEGCONF_FILE, name);
	}
	return 1;
}

static struct sniffer *sniffer_find(struct hearing *h, const char *name)
{
	for (int i = 0; i < REAC_IFSCAN_MAX; i++)
		if (h->sniff[i].name[0] && strcmp(h->sniff[i].name, name) == 0)
			return &h->sniff[i];
	return NULL;
}

/* Read a sniffer's socket dry, classify, and tell the table about the first frame that IS
 * REAC gear. Never transmits, never touches a listener, so it is safe to call from the
 * poll as well as from the socket's own callback. */
static void sniffer_drain(struct sniffer *sn)
{
	uint8_t frame[2048];
	uint64_t now = monotonic_ns();
	for (int i = 0; i < 64; i++) {
		long n = reac_capture_next(&sn->cap, frame, sizeof frame);
		if (n <= 0)
			break;
		sn->frames++;
		/* EVERY frame in the batch is offered, not just the first that classifies.
		 * One frame says REAC is here; WHO is here takes several — a desk's cfea
		 * announce comes once a second between thousands of FILLER frames, and it is
		 * the frame that decides whether this segment is ours to drive. The gate is
		 * the table's own: only an observable change earns a line, so a live wire
		 * costs a handful of lines and not 8000 a second. */
		struct reac_disco_sighting sight;
		int seen = reac_hunt_observe(&sn->hunt, frame, (size_t)n, now, &sight);
		/* ANY REAC frame cancels the masterless licence, sharper or not — this is the
		 * safety half of reac_knock.h. The wire is not empty, so it was never the case
		 * the licence is for, and the ordinary hunt rules on whatever is there.
		 *
		 * OUR OWN TRANSMISSIONS DO NOT COME BACK HERE, and it is worth saying WHY rather
		 * than trusting it: libreac's capture binds AF_PACKET to EtherType 0x8819, which
		 * registers on ptype_base, and the kernel hands locally generated OUTGOING frames
		 * to ptype_all listeners ONLY. (Measured 2026-09-09: a test capture bound to
		 * 0x8819 on the peer's own NIC counted every frame that ARRIVED and not one the
		 * peer sent.) The classifier is given this NIC's address as well — the address
		 * every emitting role of ours sources from, reac_mac.h — so a hub or a loopback
		 * that really does return our frames still cannot make us a peer of ourselves. */
		if (seen >= 0) {
			reac_knock_heard(&sn->knock, now);
			/* AND THE SAME STAMP IS WHAT THE TAP-AUTHORITY WAIT IS MEASURED FROM:
			 * "this wire has been heard" is a fact with a time on it, or it is a
			 * latch. reac_tapwait.h has the nine minutes that cost. */
			sn->last_heard_ns = now;
		}
		if (seen != 1)
			continue;
		reac_code_emit(stderr, "reac-pw", RC_S_SEGMENT_HEARD,
		        "[%s] REAC heard — %s %02x:%02x:%02x:%02x:%02x:%02x"
		        "%s%s (%u ch): this interface is a segment\n",
		        sn->name, reac_disco_role_name(sight.role),
		        sight.mac[0], sight.mac[1], sight.mac[2],
		        sight.mac[3], sight.mac[4], sight.mac[5],
		        sight.model ? " " : "", sight.model ? sight.model->display : "",
		        sight.channels);
	}
}

/* A sniffer's socket is readable. */
static void on_sniff_io(void *data, int fd, uint32_t mask)
{
	(void)fd;
	if (!(mask & SPA_IO_IN))
		return;
	sniffer_drain(data);
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

/* Did the operator answer for THIS segment? `REAC_ROLE_<segment>` is the only layer that
 * can — a bare REAC_ROLE describes every segment on the host and cannot know what is on
 * one wire. Resolved once, when the sniffer opens, so a pinned segment is served on its
 * first classifying frame rather than after a hunt it never needed: a pin is a SETTING,
 * and a daemon that made a setting wait for evidence would be second-guessing it. */
static int segment_role_pin(const char *iface, enum reac_role *out)
{
	reac_segconf_refresh(&g_segconf);
	enum reac_role_intent i;
	if (!reac_segconf_role(&g_segconf, iface, &i) || i == REAC_ROLE_INTENT_AUTO)
		return 0;
	/* `tap` PINS NO WIRE ROLE, because it presents no end of the pairing. Answering
	 * `master` here — which reac_role_from_intent would, the field holding one of two
	 * values — would make the sniffer serve a mirror port as a driving master on its
	 * first frame, which is the exact thing the eighth amendment forbids. A tap
	 * segment falls through to the ordinary listen-first path and is bound as a tap
	 * in listener_cfg_from_conf. */
	if (i == REAC_ROLE_INTENT_TAP)
		return 0;
	*out = reac_role_from_intent(i);
	return 1;
}

/* `announce` says whether this open is news. It is, at link-up: the journal line that says
 * which of the three things this interface is doing was what made the 2026-09-08 outage
 * readable. It is NOT when a refused segment re-opens a sniffer to watch its own rival —
 * that wire's story was just told, and repeating "pinned master — driving on link" under a
 * door would describe the opposite of what is happening. */
/* WHAT THE CONF ASKED THIS SEGMENT TO BE, as an INTENT — the one fact the hunt's own
 * clock needs before any listener_cfg exists. listener_cfg_from_conf resolves the same
 * key for the listener; this is the same lookup with the same two rules, read where
 * there is no listener yet. */
static enum reac_role_intent segment_role_intent(const char *iface)
{
	reac_segconf_refresh(&g_segconf);
	enum reac_role_intent i;
	if (reac_segconf_role(&g_segconf, iface, &i))
		return i;
	return REAC_ROLE_INTENT_AUTO;
}

/* WHERE THIS SEGMENT'S ROLE CAME FROM, in the words the start-up block uses. A
 * configuration whose effect cannot be read back is a configuration nobody can debug, and
 * this rig has spent a night on exactly that (spec §4). */
static const char *segment_role_source(const char *iface)
{
	return reac_segconf_find(&g_segconf, iface) &&
	       reac_segconf_find(&g_segconf, iface)->role_set
	               ? REAC_SEGCONF_FILE : "autodetected";
}

/* A `REAC_ROLE` KEY THAT IS STILL ON DISK IS NAMED, ONCE, AND NOT OBEYED. A key that
 * stopped applying and says nothing is indistinguishable from one that is working, which
 * is the same class of silence the whole spec is about — one level down. */
static void segment_say_env_role_retired(const char *iface)
{
	char v[256];
	enum reac_conf_layer layer = reac_conf_lookup("REAC_ROLE", iface, NULL, v, sizeof v);
	if (layer == REAC_CONF_NONE)
		return;
	fprintf(stderr, "reac-pw: [%s] REAC_ROLE%s%s='%s' in %s is IGNORED: a segment's role "
	        "is autodetected, and the one thing that overrides it is %s "
	        "[segment %s] role= (spec 2026-09-16)\n",
	        iface, layer == REAC_CONF_SEGMENT ? "_" : "",
	        layer == REAC_CONF_SEGMENT ? iface : "", v, reac_conf_layer_name(layer),
	        REAC_SEGCONF_FILE, iface);
}

/* IS THIS SEGMENT PINNED `tap`? The question segment_role_pin above cannot answer,
 * because `tap` deliberately pins no WIRE role — there is no end of the pairing to pin.
 * It is still a PIN, the operator answered for this wire, and since the Q5 ruling a pin
 * is what opens a door, so the hunt's clock has to be able to see one. */
static int segment_tap_pin(const char *iface)
{
	return segment_role_intent(iface) == REAC_ROLE_INTENT_TAP;
}

/* A FOREIGN DESK IS DEFERRED TO, NOT COURTED (libreac's bounded-ungranted-courtship
 * spec, "Ruling 2026-09-14: option C"; plug-and-play §4).
 *
 * Measured four times beside a real M-200 on 2026-09-12: with our COURTING slave on the
 * segment the desk's own S-1608 did not enrol in 180 s, and once the desk had granted
 * our slave it blocked that box outright while it rebooted. So a segment that hears a
 * DESK and was not told to be a recorder does not court it — it TAPS it: serve what is
 * heard, transmit nothing at all. The role that never transmits is a different role, not
 * a quieter one.
 *
 * A BOX mastering the wire is NOT this case and is joined exactly as before (seventh
 * amendment; operator 2026-09-09, "a box that wants to be master gets the clock"). A box
 * runs no handshake and grants nothing, so there is no courtship of ours to block it.
 *
 * `recorder` — REAC_ROLE_<segment>=slave — stays an EXPLICIT choice and still courts;
 * the bounded courtship exists for it. A PINNED MASTER defers too: beside a desk it is
 * the only thing it can do that is not a fight, and reac_watch re-evaluates it back to
 * its pin the moment that desk is gone (#97). */
static int segment_defers_as_tap(enum reac_role_intent intent, const struct reac_hunt *hunt)
{
	/* A BOX NEVER DEFERS (2026-09-17). The deferral exists so an AUTO segment does not
	 * court a desk that already has boxes of its own; a segment pinned `role = box` is
	 * the case where a desk on the wire is exactly who we are there for, and deferring
	 * turned it into a silent tap the moment the mixer spoke — measured in
	 * tests/box-declares-its-row.sh, where the roster read `tap` with a box pinned. A
	 * slave is already exempt for the same reason and a box is the slave end. */
	if (!hunt || intent == REAC_ROLE_INTENT_SLAVE || intent == REAC_ROLE_INTENT_TAP ||
	    intent == REAC_ROLE_INTENT_BOX)
		return 0;
	return hunt->arb.state == REAC_SEGMENT_FOREIGN && hunt->arb.rival == REAC_RIVAL_DESK;
}

static int sniffer_open_ex(struct hearing *h, const char *name, int announce)
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
	uint64_t now = monotonic_ns();
	reac_hunt_init(&sn->hunt, sn->mac, now);
	enum reac_role pin;
	int pinned = segment_role_pin(name, &pin);
	if (pinned)
		reac_hunt_pin(&sn->hunt, pin);
	sn->tap_pinned = segment_tap_pin(name);
	snprintf(sn->name, IFNAMSIZ, "%s", name);
	sn->io = pw_loop_add_io(h->loop, sn->cap.fd, SPA_IO_IN, false, on_sniff_io, sn);
	if (!sn->io) {
		reac_capture_close(&sn->cap);
		memset(sn, 0, sizeof *sn);
		return -1;
	}
	/* THE MASTERLESS OBSERVATION, on an UNPINNED wire only. A pinned interface is about
	 * to open its real listener on link anyway (reac_hunt: a pin is served on link), so
	 * it has nothing to observe. An unpinned one is watched, and taken if it stays
	 * silent — reac_knock.h has the measurement and the safety argument. */
	/* AND NEVER ON A WIRE PINNED `tap`. The licence's whole content is TAKE THE WIRE —
	 * the master role starts on that port and drives it — and a tap is the one role
	 * defined by putting nothing on the wire at all (eighth amendment). A tap pin
	 * answers for this segment, so it is served on link like any other pin and has
	 * nothing to observe. */
	if (!pinned && !sn->tap_pinned && !reac_ifscan_is_wireless(NULL, name)) {
		sn->watch_silence = 1;
		reac_knock_init(&sn->knock, now);
	}
	/* AND A WIRELESS NIC IS NEVER DRIVEN ON SILENCE, even where the operator allowlisted
	 * it into the scan (`REAC_IFACES_ALLOW_WIRELESS`). That allowlist buys LISTENING: an
	 * associated Wi-Fi interface is quiet of 0x8819 by nature, so silence there proves
	 * nothing about a REAC master and would licence a permanent 8000 fps broadcast onto
	 * somebody's access point. It is still served the moment REAC is actually heard on
	 * it, which is evidence and not a bet. */
	/* WHICH OF THE THREE THIS INTERFACE IS DOING, said once, at link. A journal that only
	 * ever says "listening for REAC" cannot distinguish a wire we are driving from a wire
	 * we are waiting on, and that is what made the 2026-09-08 outage unreadable. */
	if (!announce)
		return 0;
	/* AND WHERE THE ROLE CAME FROM, in the same line. Spec §4: a configuration whose
	 * effect cannot be read back is one nobody can debug. `(autodetected)` is the
	 * ordinary case and says so; the file is named when it answered. */
	const char *src = segment_role_source(name);
	if (sn->tap_pinned)
		fprintf(stderr, "reac-pw: [%s] listening — role tap (%s): serving what is heard "
		        "on link and transmitting NOTHING: no announce, no join, no grant, "
		        "no seglock\n", name, src);
	else if (pinned && pin == REAC_ROLE_MASTER)
		fprintf(stderr, "reac-pw: [%s] listening — role master (%s): driving on link\n",
		        name, src);
	else if (pinned)
		fprintf(stderr, "reac-pw: [%s] listening — role slave (%s): cold-connect flood, "
		        "then listening for a master\n", name, src);
	else
		fprintf(stderr, "reac-pw: [%s] listening — role auto (autodetected): the wire "
		        "decides\n", name);
	segment_say_env_role_retired(name);
	return 0;
}

static int sniffer_open(struct hearing *h, const char *name)
{
	return sniffer_open_ex(h, name, 1);
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
/* HAS THIS WIRE SHOWN US ANY REAC GEAR AT ALL? The `S_SEGMENT_HEARD` moment, which is the
 * trigger #107 names — and it is deliberately weaker than the test the HOLDER has to fail.
 *
 * ASKING FOR A RECOGNISED BOX HERE WOULD MAKE THE RULE UNREACHABLE, and that is measured,
 * not argued (auto-role amendment 2026-09-20). A cold stagebox's presence flood is
 * BROADCAST FILLER, which reac_disco's direction discipline classifies role-UNKNOWN by
 * construction, and the config-announce that makes it a `box` with a model only arrives
 * once a master is driving the wire. On the veth the segment with the box read
 * `unknown … (16 ch)` and never upgraded for as long as the budget kept its master from
 * starting — so a box-recognition trigger would fire exactly never in the deadlock it
 * exists for.
 *
 * It is still strictly more than an empty VLAN can ever show, which is what makes it
 * enough: a segment that has heard nothing takes nobody's budget, and two empty segments
 * therefore cannot trade a port. */
static int hunt_heard_reac_gear(const struct reac_hunt *hunt)
{
	return hunt && reac_hunt_heard_anything(hunt);
}

static void hearing_serve(struct hearing *h, const char *name, const struct reac_hunt *hunt)
{
	/* ONE LISTENER PER SEGMENT, AND IT IS THE ONE THAT IS ALREADY RUNNING (#108, spec
	 * amendment 2026-09-20 §a). A second listener on one wire is a second OWNER of that
	 * segment's pair by construction: it takes the next free slot, opens its own engine and
	 * publishes its own reac-capture / reac-playback carrying the SAME `reac.segment` —
	 * which is the ghost a console's segment scan reads instead of the real box. The seglock
	 * stops a second MASTER and nothing stopped the rest, so the guard belongs here, above
	 * the role. Every legitimate re-serve in this file — the refusal ending, the yield, the
	 * retake, a role reclassification — DROPS the segment first and reaches this with no
	 * listener to find. */
	if (hearing_listener(h, name)) {
		reac_code_emit(stderr, "reac-pw", RC_E_ORPHAN_PAIR,
		    "[%s] asked to serve a segment that is ALREADY served — refused, because a "
		    "second listener on one wire publishes a second node pair on the same segment. "
		    "A re-serve drops the segment first\n", name);
		return;
	}
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
	/* THE WIRE DECIDES, UNLESS SOMEONE DECIDED FOR THIS SEGMENT. `REAC_ROLE_<segment>`
	 * is an answer about THIS wire and wins outright — the role is a setting, not a
	 * guess. A bare REAC_ROLE is the FLOOR — the launch role for a segment nobody has
	 * seen yet — and the hunt has now seen it, so the floor is named and superseded
	 * rather than obeyed. That floor is what left two boxes ungranted on 2026-09-08:
	 * `REAC_ROLE=slave` in a file described every segment on the host, including the two
	 * that had nothing to slave to. */
	/* WHAT THE WIRE TURNED OUT TO BE, carried into this segment's configuration
	 * (0.5.1). The hunt classified the peer by its frame geometry; re-deriving any of
	 * that here would be a second classifier over the same evidence. */
	/* A TAP TAKES NO VERDICT FROM THE HUNT. The hunt answers "which end do we
	 * present"; a tap presents none, so a refusal or a box-master join — both of
	 * which are decisions about what WE do on the wire — describe a segment this one
	 * is not. What it heard is re-read by reac_tap itself, off the same frames. */
	/* A DESK ON THE WIRE IS DEFERRED TO (courtship ruling 2026-09-14, option C). The
	 * segment becomes a TAP here rather than a courting slave — the predicate is the one
	 * the hunt's journal line already read, so the sentence and the act agree. */
	if (!L->cfg.tap && segment_defers_as_tap(L->cfg.role_intent, hunt)) {
		L->cfg.tap = 1;
		fprintf(stderr, "reac-pw: [%s] a desk masters this segment, so it is served as a "
		        "TAP%s — never as a courting slave: with our slave present a real desk's "
		        "own S-1608 did not enrol in 180 s (2026-09-12). %s [segment %s] role=slave asks "
		        "for the recorder explicitly if that is what you want.\n", name,
		        L->cfg.role_pinned ? " (deferring its reac-pw.conf pin until this "
		                             "master is gone)" : "", REAC_SEGCONF_FILE, name);
	}
	if (L->cfg.tap) {
		/* nothing to carry */
	} else if (hunt && hunt->verdict == REAC_HUNT_HUNTING) {
		/* NOTHING DECIDES THIS WIRE YET, AND THE SEGMENT STILL EXISTS (Q5, option C).
		 * A door with no engine behind it and no refusal to report: it publishes the
		 * segment's identity and the honest `none` — nothing is mastering this wire as
		 * far as we can hear. The mechanism is the 0.5.1 door's, so there is one
		 * door in this daemon and not two. */
		L->cfg.door_only = 1;
		L->cfg.door_vacant = 1;
		L->cfg.wire_channels = 0;
	} else if (hunt && hunt->verdict == REAC_HUNT_REFUSED) {
		L->cfg.door_only = 1;
		L->cfg.wire_channels = hunt->arb.rival_channels;
		L->cfg.rival_mac_set = hunt->arb.have_mac;
		if (hunt->arb.have_mac)
			memcpy(L->cfg.rival_mac, hunt->arb.mac, 6);
	} else if (hunt && hunt->verdict == REAC_HUNT_SLAVE &&
	           hunt->arb.rival == REAC_RIVAL_BOX && hunt->arb.rival_channels > 0) {
		/* A stagebox masters this wire and we are joining it: its own width is what
		 * the segment receives and what its capture node is sized to. */
		L->cfg.join_box_master = 1;
		L->cfg.wire_channels = hunt->arb.rival_channels;
		L->cfg.rival_mac_set = hunt->arb.have_mac;
		if (hunt->arb.have_mac)
			memcpy(L->cfg.rival_mac, hunt->arb.mac, 6);
		/* AND A PIN DOES NOT SURVIVE THIS ONE (operator, 2026-09-16: enrol any box,
		 * master or slave). The generic line below leaves a pinned listener's role
		 * alone, which is right for every other verdict and wrong for this one: a
		 * MASTER engine beside a box that is already mastering serves nothing, which
		 * is what the rig measured. Joining IS taking the slave end; a `role` the
		 * join contradicts would open the wrong engine and be silent about it. */
		L->cfg.role = REAC_ROLE_SLAVE;
	}
	/* THE WIRE DECIDES WHEREVER THE FILE DID NOT. There is no FLOOR any more and no
	 * layer to name when one is superseded: a role comes from reac-pw.conf's own
	 * [segment] section or from the hunt, and nothing else can answer (spec §2). The
	 * message that used to stand here explained a bare REAC_ROLE being overruled; the
	 * key it explained is retired, so the explanation goes with it. */
	if (hunt && !L->cfg.role_pinned)
		L->cfg.role = reac_hunt_role(hunt);
	if (h->forced_rate != 0) {
		L->cfg.rxcfg.forced_rate = h->forced_rate;
		L->cfg.rate_layer = REAC_CONF_ARGV;
	}
	snprintf(L->cfg.tag, sizeof L->cfg.tag, "[%s] ", name);
	/* WHAT THIS WIRE HAS ON IT, carried into the open. The link-budget admission is
	 * the only reader (#107): a segment that has heard REAC gear may take the port
	 * from holders that have heard nothing, and one taken on the masterless licence
	 * may not. The hunt is the only thing that knows, and it dies here. */
	L->cfg.wire_heard = hunt_heard_reac_gear(hunt);
	reac_role_swap_init(&L->role_swap, L->cfg.role);
	/* listener_open cleans up after its own refusal (its contract); a feeder
	 * that will not start leaves an opened listener to close, as in main(). */
	int up = listener_open(L, h->loop) == 0;
	/* A DOOR HAS NO FEEDER. Starting one would decode the very wire this segment
	 * refused, and the refusal is total: nothing transmitted, nothing received. */
	/* A TAP'S FEEDERS ARE ALREADY RUNNING — one per stream, started inside
	 * reac_tap_start. The listener's single `rx` is not used in this role at all. */
	if (up && !L->cfg.door_only && !L->cfg.tap && reac_rx_start(&L->rx) != 0) {
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
	L->rx_started = !L->cfg.door_only && !L->cfg.tap;
	h->served++;
	if (L->cfg.door_vacant)
		/* NOT "segment up" either: nothing is running here and nothing was refused.
		 * The wire has told us nothing yet, and that is what the door says. */
		fprintf(stderr, "reac-pw: [%s] segment PUBLISHED as a VACANT DOOR — nothing is "
		        "mastering this wire that we can hear, so there is no engine behind "
		        "the node; the segment exists, its role can be set, and the first "
		        "verdict on this wire replaces the door — %lu served so far\n",
		        name, h->served);
	else if (L->cfg.door_only)
		/* NOT "segment up": nothing is running here. It is a segment that EXISTS on
		 * the graph so the refusal can be read, and the line says which. */
		fprintf(stderr, "reac-pw: [%s] segment REFUSED and PUBLISHED (door only, "
		        "%u-ch rival) — %lu served so far\n", name, L->cfg.wire_channels,
		        h->served);
	else
		/* THE ROLE THIS LINE NAMES IS THE ROLE THAT IS RUNNING. `reac_role_name` knows
		 * only the wire's two ends, so a TAP read "master" here — the role that
		 * transmits, printed over the one role that never does. `tap` is spelled by
		 * reac_role_intent_name, the vocabulary that has it. */
		reac_code_emit(stderr, "reac-pw", RC_S_SEGMENT_UP,
		        "[%s] segment up (%s%s, %s) — %lu served so far\n", name,
		        /* AND A BOX IS SPELLED `box`, for the same reason a tap is not
		         * spelled `master`: reac_role_name knows only the wire's two ends,
		         * and this line is read beside a roster that says `box`. */
		        L->cfg.tap ? reac_role_intent_name(REAC_ROLE_INTENT_TAP)
		        : L->cfg.box_model ? reac_role_intent_name(REAC_ROLE_INTENT_BOX)
		                   : reac_role_name(L->cfg.role),
		        L->cfg.join_box_master ? ", enrolling with the box that masters it" : "",
		        L->cfg.role_pinned && !(L->cfg.tap && L->cfg.role_intent != REAC_ROLE_INTENT_TAP)
		            ? "pinned by reac-pw.conf"
		        : L->cfg.tap && L->cfg.role_intent != REAC_ROLE_INTENT_TAP
		            ? "deferring to the master it heard"
		            : "chosen by hearing the wire",
		        h->served);
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
	reac_code_emit(stderr, "reac-pw", RC_S_SEGMENT_DROPPED,
	        "[%s] segment dropped — %s\n", name, why);
}

/* THE BUDGET YIELDS TO THE SEGMENT THAT HEARD A BOX (#107, auto-role amendment
 * 2026-09-20). Placed here because it acts through hearing_drop.
 *
 * `taker` has just heard a stagebox and is about to open an engine on `tx_if`. If every
 * master this daemon has on that PHYSICAL port is EMPTY — probing, no recognised box, no
 * frames arriving — those masters are flooding 97 Mbit/s each at nobody while the one
 * wire with a box on it is refused for lack of room. They drop back to listening and the
 * taker gets the port.
 *
 * ALL OR NOTHING, and that is the safety of it: one holder that is established, carrying
 * or PINNED and nothing yields, so a port with a box already on it is never opened up by
 * a second box appearing, and a pin stays the operator's answer (§2). Two passes, decide
 * then act, because the acting pass closes listeners and a decision taken over a table
 * being mutated is a decision about neither state.
 *
 * NOT A FLAP. `reac_ifscan_serve_failed` is the existing SEGMENT -> LINKED path with a
 * retry window; it bounces no port and does not touch the carrier-flap counter of the
 * 2026-09-02 amendment (b). Returns how many segments yielded. */
static int link_budget_yield(const char *taker, const char *tx_if, uint64_t now)
{
	struct hearing *h = &g_hear;
	char port[IFNAMSIZ];
	link_port_of(tx_if, port, sizeof port);
	char yielding[REAC_IFSCAN_MAX][IFNAMSIZ];
	int n = 0, holders = 0;
	for (int i = 0; i < h->n_slots; i++) {
		const struct listener *L = &h->listeners[i];
		if (!L->opened || !L->sink || !L->cfg.rxcfg.source)
			continue;
		if (strcmp(L->cfg.rxcfg.source, taker) == 0)
			continue;                /* ourselves, on the retry path */
		char theirs[IFNAMSIZ];
		link_port_of(L->cfg.tx_if, theirs, sizeof theirs);
		if (strcmp(port, theirs) != 0)
			continue;
		holders++;
		if (!listener_budget_empty(L) || L->cfg.role_pinned)
			return 0;                /* something real is on this port */
		if (n < REAC_IFSCAN_MAX)
			snprintf(yielding[n++], IFNAMSIZ, "%s", L->cfg.rxcfg.source);
	}
	if (!holders || !n)
		return 0;
	for (int i = 0; i < n; i++) {
		reac_code_emit(stderr, "reac-pw", RC_S_BUDGET_YIELDED,
		        "[%s] YIELDING %s's link budget to [%s], which has REAC gear on it — "
		        "this segment has heard nothing and is probing at nobody, and one "
		        "96 kHz master is 97%% of a 100 Mbit/s port: holding it here keeps the "
		        "wire that HAS something on it from coming up at all. Back to "
		        "listening; it takes the wire again only when it hears something "
		        "itself.\n",
		        yielding[i], port, taker);
		hearing_drop(h, yielding[i], "yielding the link budget to a segment with REAC gear on it");
		/* BACK TO LISTENING, THROUGH THE PATH THAT ALREADY EXISTS. The sniffer
		 * re-opens (LISTEN is queued) and the segment is classified again; the
		 * latch below is what stops the silence licence handing it the budget
		 * straight back. */
		reac_ifscan_serve_failed(&h->scan, yielding[i], now);
		struct sniffer *sn = sniffer_find(h, yielding[i]);
		if (sn) {
			sn->budget_yielded = 1;
			sn->budget_yield_said = 0;
		}
	}
	return n;
}

/* The definition promised above the poll. Placed here because it needs hearing_drop. */
static int listener_reopen_role_reclassify(struct listener *L, struct pw_loop *loop,
                                           enum reac_role role)
{
	(void)loop;
	if (!g_hear.enabled || !L->cfg.rxcfg.source || L->cfg.rxcfg.kind != REAC_RX_LIVE)
		return 0;               /* a --live segment: the caller swaps in place */
	char name[IFNAMSIZ];
	snprintf(name, sizeof name, "%s", L->cfg.rxcfg.source);
	if (!hearing_listener(&g_hear, name))
		return 0;               /* not one of the heard segments after all */
	fprintf(stderr, "reac-pw: [%s] REAC role -> %s: dropping the segment so the wire is "
	        "CLASSIFIED again rather than re-opened on the old verdict — a stagebox on M "
	        "is joined as one only if the hunt says so, and the hunt is what a role swap "
	        "used to skip\n", name, reac_role_name(role));
	hearing_drop(&g_hear, name, "role changed — re-hearing the wire to classify it afresh");
	/* AND THE SNIFFER'S PIN IS STALE TOO. `sniffer_open` reads `REAC_ROLE_<segment>` once
	 * and hands it to the hunt (`reac_hunt_pin`), so a sniffer that outlives the drop
	 * keeps answering with the pin the operator has just changed — measured on the veth:
	 * the segment was re-heard and classified REFUSED again, on a wire whose pin now said
	 * auto. Closing it makes the next link tick open a sniffer that reads the pin as it is
	 * now, which is the same path a cold start takes. */
	sniffer_close(&g_hear, name);
	/* AND THE WIRE HAS TO BE LISTENED TO AGAIN. A drop normally comes from a link edge,
	 * and the link brings the sniffer back on the way up; here the cable never moved, so
	 * nothing would re-open it and the segment would sit down for ever — measured. The
	 * sniffer is opened straight away with the pin as it now reads, and the scan is told
	 * this segment needs serving again, which is the same retry path a serve that failed
	 * takes. */
	sniffer_open(&g_hear, name);
	reac_ifscan_serve_failed(&g_hear.scan, name, monotonic_ns());
	return 1;
}

/* ---- TRUNK TOPOLOGY: the VLANs on a parent, and the netdevs they need ---------
 *
 * DESIGN.md's 0.5.3 contract, from openmixer's 2026-08-23-reac-trunk-vlan-daemon.md
 * §3-§5. Nothing here touches the audio path: the tap learns WHICH VLAN ids carry REAC
 * on a parent, the kernel is asked for one `<parent>.<vid>` netdev per id, and from
 * there each is an ordinary interface that the hearing above serves unchanged. */

static struct topo_tap *tap_find(struct hearing *h, const char *parent)
{
	for (int i = 0; i < REAC_TOPO_MAX_PARENTS; i++)
		if (h->tap[i].parent[0] && strcmp(h->tap[i].parent, parent) == 0)
			return &h->tap[i];
	return NULL;
}

/* ---- WHAT COUNTS AS EVIDENCE ABOUT THIS PARENT (#102) ------------------------------
 *
 * MEASURED ON THE RIG 2026-09-14, reac-pw 1.0.5. The USB NIC `enp128s20f0u2` was on a
 * DIRECT cable to a cold S-4000S-3208, its other port unplugged, and tcpdump on it saw no
 * tagged frame — no frame at all — over 25 s. Yet EVERY daemon start logged
 * `[enp128s20f0u2] tagged REAC heard — vid 11 (1 frame(s))` and the same for vid 12,
 * within 0.4 s of "pinned master — driving on link": exactly ONE frame per VLAN this
 * daemon itself masters on ANOTHER parent (`enp131s0.11/.12/.13`). The parent was then
 * refused as a trunk for ever and the cold box got no master.
 *
 * THE MECHANISM WAS THE SOCKET'S FIRST MICROSECONDS, AND THE LIBRARY CLOSED IT. Up to
 * libreac 1.2.1 `reac_topo_tap_open()` created the tap as `socket(AF_PACKET, SOCK_RAW,
 * htons(ETH_P_ALL))` and bound it a few syscalls later — the BPF filter, PACKET_AUXDATA
 * and `if_nametoindex()` sat between — and a packet socket created with a NON-ZERO
 * protocol is live on EVERY interface until its bind lands. On the rig that queued one
 * frame per VLAN per start from our own masters on ANOTHER parent, and ~10 per VLAN
 * across a link bounce. libreac 1.2.2 (#18) creates the socket with protocol 0 and gives
 * ETH_P_ALL to the bind, so the hook is never installed unbound; meson.build's floor is
 * 1.2.2 for exactly this, because the difference is invisible to a symbol check.
 *
 * THE IFINDEX TEST BELOW STAYS, AND IS NOW A REGRESSION DETECTOR. It costs one compare on
 * a frame we are already reading, it is the only thing that would announce a library that
 * went back to a wide-open tap, and it still does the work PACKET_IGNORE_OUTGOING cannot:
 * that flag drops frames as they ARRIVE, never the ones a socket already queued, and is
 * not about another interface's INBOUND traffic at all. Reading its presence as protection
 * is what kept the bug alive after #98.
 *
 * So the frame's own ifindex is the only honest answer, and the kernel puts it in
 * `sockaddr_ll.sll_ifindex` on every packet-socket read. This reads the tap directly to get
 * it — libreac's `reac_topo_tap_next()` passes no `msg_name`, so it cannot — and hands the
 * bytes to the SAME pure classifier libreac exports (`reac_topo_classify`), so no part of
 * the wire format is re-implemented here. It keeps the source MAC too: the next report of
 * this shape answers itself, because the "tagged REAC heard" line names who sent the frame
 * and which ifindex it arrived on. */
struct topo_frame {
	enum reac_topo_kind kind;
	uint16_t vid;
	uint8_t src[6];
	unsigned ifindex;       /* the kernel's answer: where this frame really came from */
	int outgoing;           /* sll_pkttype == PACKET_OUTGOING: our own transmission */
};

/* One frame off the tap. 1 = a frame, 0 = the socket is dry, -1 = error. */
static int topo_tap_read(struct reac_topo_tap *tap, struct topo_frame *f)
{
	uint8_t frame[2048];
	uint8_t control[CMSG_SPACE(sizeof(struct tpacket_auxdata))];
	struct sockaddr_ll from;
	struct iovec iov = { .iov_base = frame, .iov_len = sizeof frame };
	struct msghdr msg;
	memset(&from, 0, sizeof from);
	memset(&msg, 0, sizeof msg);
	msg.msg_name = &from;
	msg.msg_namelen = sizeof from;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof control;

	ssize_t n = recvmsg(reac_topo_tap_fd(tap), &msg, MSG_DONTWAIT);
	if (n < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;

	int tci_valid = 0;
	uint16_t tci = 0;
	for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
		if (cm->cmsg_level != SOL_PACKET || cm->cmsg_type != PACKET_AUXDATA)
			continue;
		struct tpacket_auxdata aux;
		memcpy(&aux, CMSG_DATA(cm), sizeof aux);
		/* TP_STATUS_VLAN_VALID is what separates "vid 0" from "no tag" — the kernel
		 * zeroes tp_vlan_tci for an untagged frame and a priority-tagged one alike
		 * (reac_topo.h carries the measurement). */
		if (aux.tp_status & TP_STATUS_VLAN_VALID) {
			tci_valid = 1;
			tci = aux.tp_vlan_tci;
		}
	}
	memset(f, 0, sizeof *f);
	f->ifindex = (unsigned)from.sll_ifindex;
	f->outgoing = (from.sll_pkttype == PACKET_OUTGOING);
	if ((size_t)n >= 12)
		memcpy(f->src, frame + 6, 6);   /* the source MAC, for the line that names it */
	f->kind = reac_topo_classify(frame, (size_t)n, tci_valid, tci, &f->vid);
	return 1;
}

/* TAGGED REAC ON THIS VID, SAID WHERE IT BECOMES TRUE AND NOT WHERE A NETDEV IS MADE.
 * Two facts arrive on a trunk now — this VLAN exists (any tag) and REAC rides it — and
 * tying the second to the ENSURE would make the journal depend on WHICH FRAME WON A RACE:
 * a sub-interface emits its own IPv6 multicast the moment it comes up, so on a live trunk
 * the VID is very often discovered by something that is not REAC, and a line printed only
 * at the ENSURE would then never say REAC was heard at all. Measured here on 2026-09-22:
 * segments-autodetect's arm d, where the daemon's own master traffic on farA.33 is real
 * tagged REAC and the VID was nonetheless first named by the netdev's multicast.
 *
 * So this says it once per (tap, VID), the first time a REAC frame carries that tag.
 * Idempotent, bounded, and never a source of truth about segments — see the field's note. */
static void topo_note_reac_vid(struct topo_tap *tp, uint16_t vid, const uint8_t *src)
{
	if (vid == 0)
		return;
	for (int i = 0; i < tp->reac_vids_n; i++)
		if (tp->reac_vids[i] == vid)
			return;
	if (tp->reac_vids_n >= (int)(sizeof tp->reac_vids / sizeof tp->reac_vids[0]))
		return;
	tp->reac_vids[tp->reac_vids_n++] = vid;
	const struct reac_topo_vlan *v = reac_topo_vlan_find(&tp->h->topo, tp->parent, vid);
	/* WHO SENT IT AND WHERE IT ARRIVED (#102). A verdict of one frame on a cable tcpdump
	 * says is silent cost a night; the line carries the sender's MAC and the ifindex the
	 * frame was attributed to, so the next such report is answered from the journal. */
	fprintf(stderr, "reac-pw: [%s] tagged REAC heard — vid %u (%lu frame(s)) from "
	        "%02x:%02x:%02x:%02x:%02x:%02x on ifindex %u: this parent is a TRUNK, its "
	        "VLANs are the segments\n", tp->parent, (unsigned)vid, v ? v->frames : 0UL,
	        src[0], src[1], src[2], src[3], src[4], src[5], tp->ifindex);
}

static void on_topo_io(void *data, int fd, uint32_t mask)
{
	struct topo_tap *tp = data;
	(void)fd;  /* the tap reads through its own handle; the loop only wakes us */
	if (!(mask & SPA_IO_IN))
		return;
	uint64_t now = monotonic_ns();
	for (int i = 0; i < 256; i++) {
		struct topo_frame f;
		if (topo_tap_read(&tp->tap, &f) <= 0)
			break;
		/* #102: EVIDENCE ABOUT THIS PARENT IS WHAT ARRIVED INBOUND ON THIS PARENT.
		 * With libreac >= 1.2.2 the foreign half can no longer happen (#18) and this
		 * is the alarm that would say so if it did; the outgoing half is ours to make.
		 * Anything else — another interface's frame queued before the tap was bound,
		 * or one of our own transmissions — decides nothing here. Said once per tap,
		 * with the sender, the VLAN and the real ifindex, so a repeat of this report
		 * is diagnosed from the journal alone. */
		if (f.ifindex != tp->ifindex || f.outgoing) {
			if (!tp->said_foreign) {
				tp->said_foreign = 1;
				fprintf(stderr, "reac-pw: [%s] a %s REAC frame from "
				        "%02x:%02x:%02x:%02x:%02x:%02x (vid %u, ifindex %u) reached "
				        "this parent's tap %s — it is not evidence about this "
				        "parent and decides nothing\n", tp->parent,
				        reac_topo_kind_name(f.kind),
				        f.src[0], f.src[1], f.src[2], f.src[3], f.src[4], f.src[5],
				        (unsigned)f.vid, f.ifindex,
				        f.outgoing ? "as our own transmission"
				                   : "from another interface");
			}
			continue;
		}
		if (f.kind == REAC_TOPO_TAGGED) {
			/* #98/#102: this cable carries tags NOW, and who sent the last one.
			 * TAGGED_OTHER deliberately does NOT touch any of this: a tag on some
			 * other ethertype names a VLAN and says nothing about whether this
			 * parent carries REAC, and this timestamp is what the trunk verdict
			 * reads (topo_trunk_now). Widening it would refuse a master to a cable
			 * whose only tags came from a switch. */
			tp->last_tagged_ns = now;
			tp->said_stale = 0;
			memcpy(tp->last_tagged_src, f.src, sizeof tp->last_tagged_src);
		}
		reac_topo_saw(&tp->h->topo, tp->parent, f.kind, f.vid, now);
		/* AFTER the table has counted this frame, so the line's own frame count
		 * includes the frame it is reporting: a count of 0 beside "heard" is what a
		 * detector that saw nothing would print, and a test reads it as the control. */
		if (f.kind == REAC_TOPO_TAGGED)
			topo_note_reac_vid(tp, f.vid, f.src);
	}
}

/* Watch a parent for tags. A STACKED netdev is never watched: a VLAN sub-interface has no
 * VLANs of its own, and the frames on it arrive with the tag already stripped. */
/* IS THIS NETDEV A VLAN OF A PARENT WE ARE ALREADY WATCHING?
 *
 * reac_topo_is_stacked answers the general question from `/sys/class/net/<if>/lower_*`,
 * and it answers NOT STACKED when the path cannot be read — the same answer an ordinary
 * NIC gives. That is a fail-open on a topology ACTION: measured inside the veth proof's
 * namespace, where /sys is the host's and none of the test netdevs appear in it, the
 * daemon tapped its own `trunk1.13`, read vid 13 out of the tag the kernel had just
 * stripped for it, and minted `trunk1.13.13` — a VLAN on a VLAN, with the real segment
 * then probing at a netdev nobody was on.
 *
 * The daemon does not need sysfs to know this one: `<parent>.<vid>` is the name it uses
 * itself (reac_vlan_name), so a netdev whose prefix up to the last dot is a parent in the
 * topology table IS that parent's sub-interface. A physical NIC can never match — it would
 * have to be named after a watched parent plus a suffix. Absence of the fact stays absence;
 * this only ever adds a refusal. */
static int tap_is_vlan_of_watched(struct hearing *h, const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot || dot == name || !dot[1])
		return 0;
	char parent[IFNAMSIZ];
	size_t n = (size_t)(dot - name);
	if (n >= sizeof parent)
		return 0;
	memcpy(parent, name, n);
	parent[n] = '\0';
	return reac_topo_find(&h->topo, parent) != NULL;
}

static void topo_watch_iface(struct hearing *h, const char *name)
{
	if (tap_find(h, name) || reac_topo_is_stacked(NULL, name) ||
	    tap_is_vlan_of_watched(h, name))
		return;
	struct topo_tap *tp = NULL;
	for (int i = 0; i < REAC_TOPO_MAX_PARENTS; i++)
		if (!h->tap[i].parent[0]) { tp = &h->tap[i]; break; }
	if (!tp) {
		fprintf(stderr, "reac-pw: [%s] no room for a topology tap (%d parents watched) "
		        "— a trunk on this parent would be invisible\n", name,
		        REAC_TOPO_MAX_PARENTS);
		return;
	}
	struct reac_topo_tap tap;
	if (reac_topo_tap_open(&tap, name) != 0) {
		/* NOT FATAL, AND NOT SILENT. Without the tap this interface is still sniffed
		 * and still served untagged; what is lost is the ability to SEE a trunk on
		 * it, and that has to be said or a trunk looks like an access port. */
		fprintf(stderr, "reac-pw: [%s] the topology tap could not open: %s — tagged "
		        "REAC on this parent cannot be seen (needs CAP_NET_RAW)\n",
		        name, strerror(errno));
		return;
	}
	if (reac_topo_watch(&h->topo, name) != 0) {
		/* SAID, NEVER SILENT — the table is bounded like the tap array and its
		 * refusal costs the same thing: a trunk on this parent is invisible and
		 * every VLAN on it is served as one flat segment. It returned quietly until
		 * 0.5.4, and a full table then looked exactly like a parent with no tags on
		 * it (found when the venue phase pushed a run past the bound). */
		fprintf(stderr, "reac-pw: [%s] the topology table is full (%d parents) — "
		        "a trunk on this parent would be invisible\n",
		        name, REAC_TOPO_MAX_PARENTS);
		reac_topo_tap_close(&tap);
		return;
	}
	/* THE IFINDEX THE TAP IS BOUND TO (#102), read the same way libreac read it one
	 * call ago. Without it no frame can be attributed, so a tap that cannot be named is
	 * not opened at all: the netdev went away between the open and here, and a tap
	 * counting frames it cannot attribute is exactly the defect this closes. */
	unsigned idx = if_nametoindex(name);
	if (idx == 0) {
		fprintf(stderr, "reac-pw: [%s] the topology tap opened and the interface is "
		        "already gone (%s) — no tap, so tagged REAC on this parent cannot be "
		        "seen\n", name, strerror(errno));
		reac_topo_tap_close(&tap);
		reac_topo_unwatch(&h->topo, name, monotonic_ns());
		return;
	}
	memset(tp, 0, sizeof *tp);
	snprintf(tp->parent, IFNAMSIZ, "%s", name);
	tp->linkup_ns = monotonic_ns();   /* #98: the trunk verdict is re-proved from here */
	tp->ifindex = idx;
	tp->tap = tap;
	/* OUR OWN FRAMES ARE NOT EVIDENCE ABOUT THIS PARENT, and on THIS socket they are
	 * delivered: the tap is ETH_P_ALL, and ptype_all is the one place a locally
	 * GENERATED frame turns up. Every frame this daemon puts on a `<parent>.<vid>`
	 * netdev egresses the parent TAGGED, so a segment of ours on a VLAN was proving the
	 * parent a trunk by talking — measured while writing the #98 test: a re-patched
	 * cable stayed a trunk for ever because our own minted VLAN's master kept
	 * re-proving it, and the same echo kept that VID alive in the table past its
	 * withdrawal hold. Same law as the sniffer's own MAC filter (reac_mac.h) and
	 * reac_disco's self-filter; the kernel just spells it as a sockopt here.
	 *
	 * BEST-EFFORT BY NATURE (Linux 4.20+), and reported rather than assumed: without it
	 * the classification is merely conservative — a parent keeps reading as a trunk,
	 * which refuses rather than double-delivers.
	 *
	 * IT IS SET BEFORE THE FIRST READ and it is NOT THE WHOLE ANSWER (#102). The loop
	 * source below is what makes anything read this socket, so this flag is always in
	 * place first — and it still only drops frames as they ARRIVE, never the ones the
	 * socket already queued while it was unbound, and never another interface's
	 * INBOUND traffic. on_topo_io's ifindex test is what covers both. */
	{
		int on = 1;
		if (setsockopt(reac_topo_tap_fd(&tp->tap), SOL_PACKET, PACKET_IGNORE_OUTGOING,
		               &on, sizeof on) != 0)
			fprintf(stderr, "reac-pw: [%s] the topology tap cannot ignore our own "
			        "transmissions (%s) — a VLAN of ours on this parent will keep it "
			        "classified as a trunk\n", name, strerror(errno));
	}
	tp->h = h;
	tp->io = pw_loop_add_io(h->loop, reac_topo_tap_fd(&tp->tap), SPA_IO_IN, false, on_topo_io, tp);
	if (!tp->io) {
		reac_topo_tap_close(&tp->tap);
		reac_topo_unwatch(&h->topo, name, monotonic_ns());
		memset(tp, 0, sizeof *tp);
	}
}

/* Carrier went, or the netdev did: stop listening for tags. THE TABLE IS KEPT. A netdev we
 * minted is not deleted on a link bounce — the VID simply stops arriving, and the silence
 * hold (30 s, far longer than a box power-cycle or a PHY renegotiation) is what decides
 * whether the VLAN is really gone. Deleting on carrier loss would destroy a segment that
 * the ifscan hold exists to preserve. */
static void topo_unwatch_iface(struct hearing *h, const char *name)
{
	struct topo_tap *tp = tap_find(h, name);
	if (!tp)
		return;
	if (tp->io)
		pw_loop_destroy_source(h->loop, tp->io);
	reac_topo_tap_close(&tp->tap);
	memset(tp, 0, sizeof *tp);
}

/* THE PARENT IS REALLY GONE — past the ifscan hold, or the netdev itself withdrawn — so
 * the TABLE slot goes with the tap, and the netdevs we minted on it are released: its
 * sub-interfaces carry nothing now. A link BOUNCE never comes here (that is UNLISTEN,
 * above, which keeps the table so a power-cycle does not destroy a segment).
 *
 * WHY IT EXISTS AT ALL: both bounds are 8, and until 0.5.4 neither was ever given back on
 * this path. A served interface goes LISTEN -> SERVE -> DROP and never through UNLISTEN,
 * so every segment that ever dropped kept its tap AND its table row for the life of the
 * process. Past the eighth the daemon serves every trunk as a flat segment — measured on
 * the veth proof, where the ninth interface in the run made the trunk phase's parent
 * untappable. */
static void topo_forget_iface(struct hearing *h, const char *name, uint64_t now)
{
	topo_unwatch_iface(h, name);
	reac_topo_unwatch(&h->topo, name, now);
}

/* ---- THE DECLARED SEGMENTS ------------------------------------------------ *
 *
 * A DECLARATION DOES NOT WAIT TO BE HEARD. reac_topo mints `<parent>.<vid>` when a tagged
 * REAC frame arrives on the trunk; on a cold boot no such frame can arrive, because every
 * box on the trunk is a SLAVE and a slave says nothing until a master speaks — and the
 * master cannot speak until its segment's netdev exists. Measured on the desk 2026-09-15,
 * after a reboot: every declared segment dead, every box unenrolled, no error anywhere.
 * So a segment the operator DECLARED is minted at start, and again whenever its parent
 * turns up, whatever the wire has or has not said. See reac_declared_vlan.h.
 *
 * THE PARENT MAY NOT BE THERE YET, AND THAT IS ORDINARY. The daemon can start before
 * NetworkManager has brought the trunk up, so "no such parent" is a state to come back
 * from, not a failure: it is said once and retried on every RTM_NEWLINK the interface
 * watch already delivers. */
static int declared_find(struct hearing *h, const char *parent, uint16_t vid)
{
	for (int i = 0; i < h->n_decl; i++)
		if (h->decl[i].d.vid == vid && strcmp(h->decl[i].d.parent, parent) == 0)
			return i;
	return -1;
}

static void declared_ensure_one(struct declared_seg *ds)
{
	if (ds->present)
		return;
	/* THE PARENT FIRST. A VLAN can be created over a parent that is DOWN — it only has
	 * to EXIST — which is exactly why this does not wait for carrier: a trunk whose
	 * switch port comes up a minute later must not cost the rig a minute of silence. */
	if (!if_nametoindex(ds->d.parent)) {
		if (!ds->said_waiting) {
			fprintf(stderr, "reac-pw: [%s] declared, and its parent %s is not on "
			        "this host yet — it is minted the moment the parent appears\n",
			        ds->name, ds->d.parent);
			ds->said_waiting = 1;
		}
		return;
	}

	int ours = 0;
	int present = reac_vlan_query(ds->name, &ours);
	if (present == 1) {
		if (reac_vlan_up(ds->name) != 0) {
			if (!ds->said_failed) {
				fprintf(stderr, "reac-pw: [%s] declared and present, and could "
				        "not be brought up: %s — the segment stays dead\n",
				        ds->name, strerror(errno));
				ds->said_failed = 1;
			}
			return;
		}
		ds->present = 1;
		ds->minted  = ours;   /* a leaked mint is re-owned, never left to accumulate */
		ds->said_failed = 0;
		fprintf(stderr, "reac-pw: [%s] declared: %s — up and serving\n", ds->name,
		        ours ? "re-owned, it carries our mint alias from a previous run"
		             : "adopted, the host made it and it survives us");
		return;
	}
	if (present == 0 && reac_vlan_create(ds->d.parent, ds->d.vid) == 0) {
		ds->present = 1;
		ds->minted  = 1;
		ds->said_failed = 0;
		fprintf(stderr, "reac-pw: [%s] declared and absent: created over %s "
		        "(marked %s) — it goes when we do\n",
		        ds->name, ds->d.parent, REAC_VLAN_ALIAS);
		return;
	}
	/* REPORT, NEVER FAIL DEAF, and name the one command that fixes it. */
	if (!ds->said_failed) {
		int e = errno;
		fprintf(stderr, "reac-pw: [%s] is DECLARED and cannot be created (%s)%s — "
		        "this segment is dead until it is. Either grant the capability, or:\n"
		        "         ip link add link %s name %s type vlan id %u && ip link set %s up\n",
		        ds->name, strerror(e), e == EPERM ? " — CAP_NET_ADMIN is missing" : "",
		        ds->d.parent, ds->name, (unsigned)ds->d.vid, ds->name);
		ds->said_failed = 1;
	}
}

/* Every declared segment, every time. Cheap: one if_nametoindex per entry that is not
 * already up, and nothing at all for one that is. */
static void declared_ensure(struct hearing *h)
{
	for (int i = 0; i < h->n_decl; i++) {
		struct declared_seg *ds = &h->decl[i];
		/* A NETDEV THAT WENT AWAY IS NOT STILL PRESENT. The parent can be unplugged
		 * and re-minted by the host, or someone can delete the sub-interface; the
		 * cheap re-check is what makes the next pass put it back. */
		if (ds->present && reac_vlan_query(ds->name, NULL) != 1) {
			fprintf(stderr, "reac-pw: [%s] declared segment's netdev is GONE — "
			        "re-ensuring it\n", ds->name);
			ds->present = 0;
			ds->said_waiting = 0;
		}
		declared_ensure_one(ds);
	}
}

/* Read the declarations once, at start. */
static void declared_load(struct hearing *h)
{
	/* A DECLARATION IS A SECTION IN THE OVERRIDE FILE, NOT A KEY NAME (spec §2).
	 * reac_declared_vlan read `REAC_ROLE_<parent>.<vid>`'s NAME to mint a netdev before
	 * anything was heard — the cold-boot fix of 2026-09-15, which is real and is kept —
	 * but the declaration then outlived what declared it: three master VLANs were left
	 * standing on a 100 Mbit port with no box on any of them (auto-role §5d). Naming a
	 * segment in this file is an explicit act; a role projection is a side effect. */
	struct reac_declared_vlan tab[REAC_DECLARED_VLAN_MAX];
	int count = 0;
	int n = reac_segconf_declared(&g_segconf, tab, REAC_DECLARED_VLAN_MAX, &count);
	if (n >= 0)
		n = count;
	if (n < 0) {
		h->decl_full = 1;
		n = REAC_DECLARED_VLAN_MAX;
		fprintf(stderr, "reac-pw: more than %d declared VLAN segments — the rest are "
		        "NOT minted (bounded, reported)\n", REAC_DECLARED_VLAN_MAX);
	}
	for (int i = 0; i < n && i < REAC_DECLARED_VLAN_MAX; i++) {
		struct declared_seg *ds = &h->decl[h->n_decl];
		memset(ds, 0, sizeof *ds);
		ds->d = tab[i];
		if (reac_vlan_name(ds->d.parent, ds->d.vid, ds->name, sizeof ds->name) != 0) {
			fprintf(stderr, "reac-pw: declared segment %s.%u does not fit in an "
			        "interface name — it cannot be served\n",
			        ds->d.parent, (unsigned)ds->d.vid);
			continue;
		}
		h->n_decl++;
	}
	if (h->n_decl == 0)
		return;
	fprintf(stderr, "reac-pw: %d declared VLAN segment(s):", h->n_decl);
	for (int i = 0; i < h->n_decl; i++)
		fprintf(stderr, " %s", h->decl[i].name);
	fprintf(stderr, " — each is minted and brought up now, and again whenever its "
	        "parent appears\n");
}

/* The exit's half of the contract, beside reac_topo's. */
static void declared_release_all(struct hearing *h)
{
	for (int i = 0; i < h->n_decl; i++) {
		struct declared_seg *ds = &h->decl[i];
		if (!ds->minted)
			continue;
		if (reac_vlan_delete(ds->name) == 0)
			fprintf(stderr, "reac-pw: [%s] removed — it was declared and we created "
			        "it, so we take it away\n", ds->name);
		else
			fprintf(stderr, "reac-pw: [%s] was ours and could not be removed: %s — "
			        "the next start re-owns it by its alias\n",
			        ds->name, strerror(errno));
		ds->minted = 0;
		ds->present = 0;
	}
}

/* One ENSURE: adopt what is there, create what is not, and say which. */
static void topo_ensure(struct hearing *h, const char *parent, uint16_t vid, uint64_t now)
{
	const struct reac_topo_vlan *v = reac_topo_vlan_find(&h->topo, parent, vid);
	/* THIS LINE IS ABOUT A VLAN EXISTING, AND SINCE libreac 1.5.0 THAT IS ALL IT MAY
	 * CLAIM. A tag on ANY ethertype names a VLAN — that is how a COLD one is found, and on
	 * a cold rig it is the only evidence there is (the 2026-09-16 spec, amendment
	 * 2026-09-22). Whether REAC rides this VID is a different fact, said by
	 * topo_note_reac_vid() at the frame that makes it true; saying it here would depend on
	 * which frame won a race, and over an LLDP frame it would be the #102 report again with
	 * the evidence INVENTED rather than misattributed.
	 *
	 * THE FRAME COUNT IS THE CONTROL AND IT STAYS: a detector that saw nothing cannot
	 * print one, which is what segments-autodetect's arm d reads. */
	fprintf(stderr, "reac-pw: [%s] tagged VLAN heard — vid %u (%lu frame(s)): a tag names "
	        "a VLAN whatever carries it, so this VID is a segment of this trunk%s\n",
	        parent, (unsigned)vid, v ? v->frames : 0UL,
	        reac_topo_is_trunk(&h->topo, parent) ? ""
	                : " — nothing REAC has been heard on this parent yet, which is exactly"
	                  " what a cold rig looks like");

	char name[IFNAMSIZ];
	if (reac_vlan_name(parent, vid, name, sizeof name) != 0) {
		fprintf(stderr, "reac-pw: [%s] vid %u: `%s.%u` does not fit in an interface "
		        "name — this VLAN is heard and cannot be served\n",
		        parent, (unsigned)vid, parent, (unsigned)vid);
		reac_topo_ensure_failed(&h->topo, parent, vid, now);
		return;
	}
	/* A DECLARED SEGMENT IS NOT THE HEARD TABLE'S TO OWN. Its netdev already exists —
	 * declared_ensure made it before any frame arrived — and its lifetime is the
	 * daemon's, not this VID's silence hold. Recorded as ADOPTED (`minted = 0`), which
	 * is exactly the flag that stops reac_topo_release deleting it 30 s into a quiet
	 * show; declared_release_all is what removes it, and only if we minted it. */
	if (declared_find(h, parent, vid) >= 0) {
		reac_topo_ensured(&h->topo, parent, vid, 0);
		fprintf(stderr, "reac-pw: [%s] vid %u: %s was already up — it is DECLARED, so "
		        "it is served and its netdev outlives any silence on this VID\n",
		        parent, (unsigned)vid, name);
		return;
	}
	int ours = 0;
	int present = reac_vlan_query(name, &ours);
	if (present == 1) {
		if (reac_vlan_up(name) != 0)
			fprintf(stderr, "reac-pw: [%s] %s exists and could not be brought up: %s\n",
			        parent, name, strerror(errno));
		reac_topo_ensured(&h->topo, parent, vid, ours);
		if (ours)
			fprintf(stderr, "reac-pw: [%s] vid %u: re-owned %s — it carries our mint "
			        "alias, so a previous run leaked it; it is ours again and goes "
			        "when we do\n", parent, (unsigned)vid, name);
		else
			fprintf(stderr, "reac-pw: [%s] vid %u: adopted %s — the host made it, it "
			        "survives us untouched\n", parent, (unsigned)vid, name);
		return;
	}
	if (present == 0 && reac_vlan_create(parent, vid) == 0) {
		reac_topo_ensured(&h->topo, parent, vid, 1);
		fprintf(stderr, "reac-pw: [%s] vid %u: created %s (marked %s) — serving it as a "
		        "segment\n", parent, (unsigned)vid, name, REAC_VLAN_ALIAS);
		return;
	}
	/* REPORT, NEVER FAIL DEAF (§4e). The VID goes on being heard and the ensure is
	 * retried on a window; an operator who reads this line knows exactly which VLAN is
	 * unserved and the one command that fixes it. */
	int e = errno;
	reac_topo_ensure_failed(&h->topo, parent, vid, now);
	fprintf(stderr, "reac-pw: [%s] vid %u: %s cannot be created (%s)%s — the VLAN is "
	        "HEARD and NOT SERVED. Either grant the capability, or:\n"
	        "         ip link add link %s name %s type vlan id %u && ip link set %s up\n",
	        parent, (unsigned)vid, name, strerror(e),
	        e == EPERM ? " — CAP_NET_ADMIN is missing" : "",
	        parent, name, (unsigned)vid, name);
}

/* One RELEASE. §4d in one branch: what we minted, we remove; what we adopted was never
 * ours and is left exactly as it was found. */
static void topo_release(struct hearing *h, const char *parent, uint16_t vid, int minted)
{
	(void)h;
	char name[IFNAMSIZ];
	if (reac_vlan_name(parent, vid, name, sizeof name) != 0)
		return;
	if (!minted) {
		fprintf(stderr, "reac-pw: [%s] left alone — the host made it, so it is not "
		        "ours to remove\n", name);
		return;
	}
	if (reac_vlan_delete(name) == 0)
		fprintf(stderr, "reac-pw: [%s] removed — we created it, so we take it away\n", name);
	else
		fprintf(stderr, "reac-pw: [%s] was ours and could not be removed: %s — it is "
		        "left behind, and the next start will re-own it by its alias\n",
		        name, strerror(errno));
}

/* Do what the topology table says. Main loop only, like hearing_apply. */
/* IS THIS PARENT A TRUNK RIGHT NOW? (#98)
 *
 * `reac_topo_is_trunk` answers "has tagged REAC EVER been heard here", which is the right
 * question for the table (a VLAN's netdev must outlive a box power-cycle) and the wrong
 * one for a refusal. A refusal has to be about the cable as it is: the trunk verdict is
 * believed while a tag has been heard since this link came up, and for one re-proof
 * window after link-up so a freshly-linked trunk is not served untagged in the gap before
 * its first tagged frame. Past that window with no tag, the parent is an ordinary segment
 * again and is served as one — which is what a re-patched cable is.
 *
 * The window compare is written so it cannot wrap: unsigned time subtracts in the right
 * order or not at all (the same trap reac_watch_decide's dwell documents). */
static int topo_trunk_now(struct hearing *h, const char *parent, uint64_t now)
{
	if (!reac_topo_is_trunk(&h->topo, parent))
		return 0;
	struct topo_tap *tp = tap_find(h, parent);
	/* NO TAP OF OUR OWN means we cannot see tags at all, and absence of the fact is
	 * not evidence against it: the table's answer stands. */
	if (!tp)
		return 1;
	/* A TAG HEARD RECENTLY, NOT A TAG HEARD ONCE (#102). This was `tagged_since_linkup`,
	 * a latch, and one misattributed frame at start-up therefore pinned the verdict for
	 * the life of the process — #98's re-proof could never run on the start path, which
	 * is the half of #102 that survives even with the attribution fixed. A real trunk
	 * re-proves itself thousands of times a second, so the rolling window costs it
	 * nothing. */
	if (tp->last_tagged_ns) {
		if (now <= tp->last_tagged_ns)
			return 1;
		if (now - tp->last_tagged_ns < REACPW_TRUNK_RECLASSIFY_NS)
			return 1;
		return 0;
	}
	/* NOT ONE TAG YET: the grace after this cable came up (and after start, which is
	 * the same stamp), so a freshly-linked trunk is not served untagged in the gap
	 * before its first tagged frame. */
	if (now <= tp->linkup_ns)
		return 1;
	return now - tp->linkup_ns < REACPW_TRUNK_RECLASSIFY_NS;
}

static void topo_apply(struct hearing *h, uint64_t now)
{
	/* THE PARENT OF A TRUNK IS NOT A SEGMENT (§3, fact B). Said once, and acted on: a
	 * listener that got in before the verdict is dropped, and hearing_hunt will not
	 * elect a role on it again. */
	for (int i = 0; i < REAC_TOPO_MAX_PARENTS; i++) {
		struct topo_tap *tp = &h->tap[i];
		if (!tp->parent[0] || !reac_topo_is_trunk(&h->topo, tp->parent))
			continue;
		/* #98: the verdict is stale — this cable has carried no tag since it came up,
		 * so it is not a trunk any more and nothing on it is refused for being one. */
		if (!topo_trunk_now(h, tp->parent, now)) {
			if (!tp->said_stale) {
				tp->said_stale = 1;
				/* SAYABLE AGAIN AFTERWARDS (#102): a tag heard later clears this and
				 * re-arms said_trunk, so the journal alternates honestly instead of
				 * printing one verdict for the life of the process. */
				tp->said_trunk = 0;
				fprintf(stderr, "reac-pw: [%s] tagged REAC was heard on this parent "
				        "before, and NOT ONCE since — nothing tagged in the last "
				        "%llu ms, so the cable has been re-purposed: it is an "
				        "ordinary segment again and untagged REAC on it is served\n",
				        tp->parent,
				        (unsigned long long)(REACPW_TRUNK_RECLASSIFY_NS / 1000000ULL));
			}
			continue;
		}
		if (!tp->said_trunk) {
			tp->said_trunk = 1;
			fprintf(stderr, "reac-pw: [%s] this parent carries tagged REAC, so it is "
			        "not itself a segment — the kernel hands us its VLANs untagged "
			        "and each of those is one\n", tp->parent);
		}
		if (hearing_listener(h, tp->parent))
			hearing_drop(h, tp->parent, "it is a trunk parent, and its VLANs are the "
			             "segments");
		/* §4f's one refusal: untagged REAC on a parent that also carries tagged REAC
		 * is not served. Driving it would put a master on the parent while masters run
		 * on its sub-interfaces, which is the fact-B double-delivery fault. */
		if (reac_topo_untagged_on_trunk(&h->topo, tp->parent))
			fprintf(stderr, "reac-pw: [%s] untagged REAC on a trunk's native VLAN is "
			        "not served; give it a tag\n", tp->parent);
	}

	reac_topo_tick(&h->topo, now);
	struct reac_topo_event ev;
	while (reac_topo_next(&h->topo, &ev)) {
		if (ev.verb == REAC_TOPO_ENSURE)
			topo_ensure(h, ev.parent, ev.vid, now);
		else if (ev.verb == REAC_TOPO_RELEASE)
			topo_release(h, ev.parent, ev.vid, ev.minted);
	}
}

/* Do what the table says. Main loop only. */
static void hearing_apply(struct hearing *h)
{
	struct reac_ifscan_event ev;
	while (reac_ifscan_next(&h->scan, &ev)) {
		const struct reac_ifscan_entry *e = reac_ifscan_find(&h->scan, ev.name);
		switch (ev.verb) {
		case REAC_IFSCAN_LISTEN:
			/* AN IGNORED SEGMENT IS NEVER TOUCHED, and the refusal is where the
			 * work would have started: no sniffer, no topology tap, no listener,
			 * no netdev. This is trunk spec §8's REAC_IFACES_DENY, finally built,
			 * in the place the 2026-09-16 ruling puts it — the operator's own
			 * file, one section per segment. */
			if (segment_ignored(h, ev.name))
				break;
			sniffer_open(h, ev.name);
			/* AND THE TAG DETECTOR, on a physical parent. The sniffer answers
			 * "is there REAC here"; only this answers "is it tagged, and on
			 * which VLAN", because the parent's protocol-bound socket cannot
			 * tell the two apart (reac_topo.h's measurement). */
			topo_watch_iface(h, ev.name);
			break;
		case REAC_IFSCAN_UNLISTEN:
			sniffer_close(h, ev.name);
			topo_unwatch_iface(h, ev.name);
			fprintf(stderr, "reac-pw: [%s] link down — no longer listening\n", ev.name);
			break;
		case REAC_IFSCAN_SERVE: {
			/* The verdict is read BEFORE the sniffer that holds it may be closed — the
			 * hunt normally dies with its sniffer, and the role it elected is the one
			 * thing the listener needs out of it.
			 *
			 * A WIRE WE TOOK AND NOBODY PINNED KEEPS BOTH (reac_watch.h). The role we
			 * elected on it is ours only while nobody else claims it, and the thing
			 * that could claim it — a desk, a box switched to M — can only ever turn
			 * up later. So the sniffer lives on beside the listener and hearing_yield
			 * reads it, on a wire won by hearing a box exactly as on one won by
			 * proving silence: 0.5.0 kept only the second, and the venue case is the
			 * first. A refused wire keeps its sniffer for the same reason (0.5.1):
			 * a door has no engine that could ever notice the rival leaving. Two
			 * AF_PACKET sockets on one interface cost one more idle fd. */
			if (segment_ignored(h, ev.name))
				break;
			struct sniffer *sn = sniffer_find(h, ev.name);
			struct reac_hunt verdict;
			int have = sn != NULL;
			int keep = have && reac_watch_keep(sn->hunt.verdict, sn->hunt.pinned);
			if (have)
				verdict = sn->hunt;
			if (!keep)
				sniffer_close(h, ev.name);
			hearing_serve(h, ev.name, have ? &verdict : NULL);
			break;
		}
		case REAC_IFSCAN_DROP:
			hearing_drop(h, ev.name, e ? "link down past the hold, or the interface went away"
			                           : "the interface went away");
			/* A retained sniffer dies with the segment it was watching, or the next
			 * link-up finds one already open and opens no fresh hunt. */
			sniffer_close(h, ev.name);
			/* AND SO DOES ITS TOPOLOGY TAP, or the taps LEAK — one per segment that
			 * ever drops. Only UNLISTEN released them, and a SERVED interface never
			 * passes through UNLISTEN: it goes LISTEN -> SERVE -> DROP, so its tap
			 * outlived it every time. The bound is REAC_TOPO_MAX_PARENTS, and past it
			 * the daemon says "no room for a topology tap ... a trunk on this parent
			 * would be invisible" and every later trunk is served as a flat segment.
			 * Found by the 0.5.4 venue phase: one more segment in the run and the
			 * trunk phase's parent could not be tapped at all. The next link-up opens
			 * a fresh tap, exactly as it opens a fresh sniffer. */
			topo_forget_iface(h, ev.name, monotonic_ns());
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
	/* THE PARENT MAY HAVE JUST APPEARED. The interface watch's own RTM_NEWLINK is the
	 * event that says so, and it is already being drained here — a declared VLAN needs
	 * no second netlink socket to be re-ensured on it. */
	if (h->decl_recheck) {
		h->decl_recheck = 0;
		declared_ensure(h);
	}
}

static void on_hearing_nl_io(void *data, int fd, uint32_t mask)
{
	(void)fd;
	struct hearing *h = data;
	if (mask & SPA_IO_IN) {
		reac_ifscan_drain(&h->scan, monotonic_ns());
		/* An RTM_NEWLINK arrived: a parent may have appeared. hearing_apply, on the
		 * poll, is where the ensure is actually done — every verb this block owns is
		 * applied from the poll, never from inside a callback. */
		h->decl_recheck = 1;
	}
}

/* THE HUNT'S OWN CLOCK. Evidence arrives in the sniffer's io callback; the DECISION is
 * taken here, on the 200 ms poll, for two reasons. A window that only advances when a
 * frame arrives cannot expire on a wire that has gone quiet — which is the case it
 * exists to answer. And `reac_ifscan_heard` is what queues a SERVE, so calling it from
 * inside a sniffer's own callback would arrange for that sniffer to be destroyed from
 * within itself; every verb this block owns is applied from the poll for exactly that
 * reason.
 *
 * Every verdict but HUNTING now turns the interface into a segment. MASTER and SLAVE
 * bring an engine up; a REFUSAL (a box on a wire pinned master, or an unreadable rival)
 * brings up a DOOR — a node carrying the refusal props and nothing else — because a
 * refusal nobody can see is indistinguishable from a daemon that is not running, which
 * is exactly what the 2026-09-09 rig proof produced. Refused still means never joined,
 * never probed at, never fought. */
static void hearing_hunt(struct hearing *h, uint64_t now)
{
	for (int i = 0; i < REAC_IFSCAN_MAX; i++) {
		struct sniffer *sn = &h->sniff[i];
		if (!sn->name[0])
			continue;
		const struct reac_ifscan_entry *e = reac_ifscan_find(&h->scan, sn->name);
		if (!e || e->state != REAC_IFSCAN_LINKED)
			continue;   /* already a segment, or on the serve-failed retry hold */
		/* THE WIRE IS ASKED HERE, NOT THE SCHEDULER. Both questions below — the
		 * tap-authority wait and the masterless licence — are answered from
		 * `last_heard_ns`, a stamp only this socket's callback advances. Serving a
		 * segment runs to completion inside this same loop, and serving one pinned box
		 * takes about 530 ms: longer than REAC_KNOCK_LISTEN_NS. So the frames a busy
		 * wire delivered during that work sit unread in the socket queue while the
		 * licence's window closes over them, and the daemon takes a wire that a box
		 * master is filling at 2000 fps (measured 2026-09-22: 349 frames read, then the
		 * same 349 across a 530 ms poll gap, then DRIVE, then the yield back to SLAVE
		 * the instant the queue was finally drained).
		 *
		 * Reading the socket at the point of decision closes it for good: every frame
		 * that arrived is either already stamped by the callback or is still queued and
		 * is stamped now, so silence here IS the wire's silence. */
		sniffer_drain(sn);
		/* A TRUNK PARENT IS NOT A SEGMENT. It receives every sub-interface's frames
		 * with the tag gone (reac_topo.h, fact B), so a role elected here would be
		 * elected over another VLAN's box and answered UNTAGGED onto the native
		 * VLAN — two masters for one box. Its VLANs are the segments, and they are
		 * hunted on their own sniffers like any other interface.
		 *
		 * ON A TAPPED PARENT THE TAP IS THE AUTHORITY, and this is a RACE CLOSED
		 * RATHER THAN NARROWED. The sniffer and the tap are two sockets on one
		 * netdev fed the SAME frames; the sniffer simply cannot tell a tagged frame
		 * from an untagged one. So a sighting the tap has not classified YET is a
		 * sighting whose VLAN is still unknown, and electing a role on it would serve
		 * a trunk parent for as long as it took the tap to catch up — measured once
		 * as a segment on a trunk parent that was then dropped under it. Waiting for
		 * the tap costs one poll on an access port, where the very same frame is
		 * already queued on both sockets, and nothing at all where no tap exists (a
		 * sub-interface, or a tap that could not open — both said in the journal). */
		const struct reac_topo_parent *tp = reac_topo_find(&h->topo, sn->name);
		/* #98: `tagged > 0` is "a tag was EVER heard here", and a re-patched cable
		 * made that a permanent refusal. The hunt asks about the wire as it is now,
		 * so it reads the same freshness topo_apply does. */
		if (tp && tp->tagged > 0 && topo_trunk_now(h, sn->name, now))
			continue;
		/* AND THE WAIT IS BOUNDED, because the tap can only place a frame that KEEPS
		 * ARRIVING. This was `reac_hunt_heard_anything`, an EVER question, and on
		 * 2026-09-21 a frame belonging to the S-1608 on ANOTHER interface was
		 * misattributed to this sniffer in the instant it opened: `untagged` stayed 0
		 * because the tap never saw that frame, "heard anything" stayed 1 for the life
		 * of the process, and a direct cable with a cold S-0808 on it was skipped here
		 * for nine minutes while carrying 0 RX packets. reac_tapwait.h holds the
		 * journal and the rule — a sighting binds while it is FRESH, and a wire that
		 * has gone silent goes back to the masterless observation below. */
		const struct reac_tapwait_in tw = {
			.tapped = tp != NULL,
			.untagged = tp ? tp->untagged : 0,
			.last_heard_ns = sn->last_heard_ns,
			.now_ns = now,
		};
		if (reac_tapwait_binds(&tw))
			continue;
		if (e->retry_after_ns != 0 && now < e->retry_after_ns)
			continue;

		/* A TAP PIN IS A PIN, AND A PIN IS SERVED ON LINK (arbitration §6 Q5,
		 * ANSWERED 2026-09-14, option C). It waits for no frame, for the same reason
		 * reac_hunt's own pin does not: the operator answered for this wire, and a
		 * mirror port with nothing on it yet is the case the ruling is ABOUT. Nothing
		 * is transmitted by taking it — that is what the role means — so there is no
		 * evidence to require before acting on it. The listener then serves whatever
		 * the survey hears, and a VACANT DOOR when it hears nothing. */
		if (sn->tap_pinned) {
			if (!sn->tap_served) {
				sn->tap_served = 1;
				fprintf(stderr, "reac-pw: [%s] %s [segment %s] role pins this segment as "
				        "TAP — serving on link with no frame waited for: a tap "
				        "asserts nothing, so there is nothing for the wire to "
				        "agree with\n", sn->name, REAC_SEGCONF_FILE, sn->name);
			}
			reac_ifscan_heard(&h->scan, sn->name, now);
			continue;
		}

		/* THE MASTERLESS OBSERVATION, then the decision. A wire nobody pinned that has
		 * carried not one frame for REAC_KNOCK_LISTEN_NS has no master on it, so it may
		 * be DRIVEN — the hunt's own licence, granted here and ruled on below like any
		 * other input. It is not a transmission of its own: the port is taken through
		 * the ordinary master role, with the ordinary pacer and this NIC's own address,
		 * because a cold box answers a master that is driving and not a lone announce
		 * (measured on the rig with 0.5.0-3: two knocks in six seconds, rx +0 for over a
		 * minute; the pinned path brought the same box up in two seconds). */
		if (sn->watch_silence && reac_knock_step(&sn->knock, now) == REAC_KNOCK_ACT_DRIVE) {
			reac_hunt_silence_proven(&sn->hunt);
			fprintf(stderr, "reac-pw: [%s] no REAC heard in %llu ms — a master fills "
			        "every slot, so this wire has none: taking it as MASTER and probing "
			        "until a box cold-connects\n", sn->name,
			        (unsigned long long)(REAC_KNOCK_LISTEN_NS / 1000000ULL));
		}

		int changed = reac_hunt_step(&sn->hunt, now);
		switch (sn->hunt.verdict) {
		case REAC_HUNT_SLAVE:
			if (changed && sn->hunt.arb.rival == REAC_RIVAL_BOX)
				/* THE ORDINARY CASE, not a tolerated hazard (operator, 2026-09-09).
				 * A clock is a clock whichever end of the pairing sends it. */
				fprintf(stderr, "reac-pw: [%s] box masters this wire — joining it as a "
				        "slave (operator rule: a box that wants to be master gets the "
				        "clock): %02x:%02x:%02x:%02x:%02x:%02x at %u ch, so this "
				        "segment follows its clock and takes what it broadcasts\n",
				        sn->name,
				        sn->hunt.arb.mac[0], sn->hunt.arb.mac[1], sn->hunt.arb.mac[2],
				        sn->hunt.arb.mac[3], sn->hunt.arb.mac[4], sn->hunt.arb.mac[5],
				        sn->hunt.arb.rival_channels);
			else if (changed && sn->hunt.pinned)
				fprintf(stderr, "reac-pw: [%s] %s [segment %s] role pins this segment as SLAVE — "
				        "opening the slave side on link, without waiting to be heard\n",
				        sn->name, REAC_SEGCONF_FILE, sn->name);
			else if (changed)
				/* AND THE SENTENCE IS THE ACT. This line said "joining it as SLAVE"
				 * over a segment that goes on to be served as a TAP — the two were
				 * decided in different functions and drifted apart. One predicate
				 * answers both now (segment_defers_as_tap), read here and at the
				 * serve, so the journal cannot describe a role the daemon does not
				 * take. */
				fprintf(stderr, "reac-pw: [%s] a desk masters this segment "
				        "(%02x:%02x:%02x:%02x:%02x:%02x) — %s\n", sn->name,
				        sn->hunt.arb.mac[0], sn->hunt.arb.mac[1], sn->hunt.arb.mac[2],
				        sn->hunt.arb.mac[3], sn->hunt.arb.mac[4], sn->hunt.arb.mac[5],
				        segment_defers_as_tap(segment_role_intent(sn->name), &sn->hunt)
				          ? "TAPPING it: serving what it broadcasts and transmitting "
				            "nothing at all, because a courting slave of ours keeps a "
				            "desk's own boxes from enrolling"
				          : "joining it as SLAVE and following its pace");
			reac_ifscan_heard(&h->scan, sn->name, now);
			break;
		case REAC_HUNT_MASTER:
			if (changed && sn->hunt.pinned)
				fprintf(stderr, "reac-pw: [%s] %s [segment %s] role pins this segment as MASTER — "
				        "driving on link, with no frame waited for (a cold box has none "
				        "to give)\n", sn->name, REAC_SEGCONF_FILE, sn->name);
			else if (changed)
				/* TWO ROADS REACH THIS VERDICT AND THEY ARE NOT THE SAME FACT.
				 * A box HEARD on the wire is one; a wire PROVEN SILENT is the
				 * other (reac_knock's licence, `silence_proven`), and this
				 * sentence claimed the first in both cases. Read straight off
				 * the desk's journal, 2026-09-16 14:38:56, two lines apart:
				 *
				 *   no REAC heard in 500 ms — ... this wire has none: taking it
				 *   as MASTER and probing until a box cold-connects
				 *   no master heard in 3 s and a box is present — taking the
				 *   wire as MASTER: probe, grant, establish
				 *
				 * Nothing had been heard at all. An operator reading the second
				 * line believes the daemon can see a box, which is the one thing
				 * it could not see for the next 73 minutes. */
				fprintf(stderr, "reac-pw: [%s] no master heard in %llu s and %s — "
				        "taking the wire as MASTER: probe, grant, establish\n",
				        sn->name,
				        (unsigned long long)(REAC_HUNT_WINDOW_NS / 1000000000ULL),
				        sn->hunt.silence_proven
				          ? "NOTHING at all was heard on it — a master fills every "
				            "audio slot, so this wire has none. Whether a box is "
				            "there is still unknown: a slave is silent until a "
				            "master speaks"
				          : "a box is present");
			/* THE PIN DECIDES WHETHER WE KEEP WATCHING IT, not the evidence we won
			 * it with (0.5.4). A wire nobody answered for is ours only while nobody
			 * else claims it, however we came to be driving it; a pinned one keeps
			 * its role. `driven_on_silence` survives as the wording of the yield. */
			sn->watched = reac_watch_keep(REAC_HUNT_MASTER, sn->hunt.pinned);
			sn->driven_on_silence = sn->hunt.silence_proven && !sn->hunt.pinned;
			/* A SEGMENT THAT YIELDED ITS PORT'S BUDGET DOES NOT TAKE IT BACK ON
			 * THE SAME SILENCE (#107). The licence that won this wire is an
			 * argument about an EMPTY wire, and it is just as true after the
			 * yield as before it — so re-serving here would refuse on the
			 * budget every 5 s for ever, or, with two empty segments on one
			 * port, hand it back and forth. The latch is cleared by this
			 * segment's OWN evidence and by nothing else. */
			if (sn->budget_yielded && !reac_hunt_heard_anything(&sn->hunt)) {
				if (!sn->budget_yield_said) {
					sn->budget_yield_said = 1;
					fprintf(stderr, "reac-pw: [%s] still LISTENING — this "
					        "segment yielded its port's link budget to a "
					        "segment with REAC gear on it, and nothing has "
					        "been heard here since; it takes the wire again on "
					        "the first frame of its own, never on silence\n",
					        sn->name);
				}
				break;
			}
			sn->budget_yielded = 0;
			reac_ifscan_heard(&h->scan, sn->name, now);
			break;
		case REAC_HUNT_REFUSED:
			if (changed && sn->hunt.arb.rival == REAC_RIVAL_BOX)
				reac_code_emit(stderr, "reac-pw", RC_E_ENROLL_REFUSED,
				        "[%s] REFUSED (%s): %s [segment %s] role pins this "
				        "segment MASTER and %02x:%02x:%02x:%02x:%02x:%02x masters it at "
				        "%u ch, a BOX width. Two answers, and the console never fights a "
				        "box: set the box's REAC Mode switch to slave and power-cycle it, "
				        "or drop the pin and this wire will JOIN it. Nothing is "
				        "transmitted here and nothing is fought — the segment is "
				        "published as a door so the refusal can be seen.\n",
				        sn->name, reac_rival_refusal(sn->hunt.arb.rival),
				        REAC_SEGCONF_FILE, sn->name,
				        sn->hunt.arb.mac[0], sn->hunt.arb.mac[1], sn->hunt.arb.mac[2],
				        sn->hunt.arb.mac[3], sn->hunt.arb.mac[4], sn->hunt.arb.mac[5],
				        sn->hunt.arb.rival_channels);
			else if (changed)
				reac_code_emit(stderr, "reac-pw", RC_E_ENROLL_REFUSED,
				        "[%s] REFUSED (%s): "
				        "%02x:%02x:%02x:%02x:%02x:%02x masters this wire and carries no "
				        "legal 52 + n*36 geometry, so there is nothing to size a segment "
				        "from and nobody has captured a peer like it. Neither driven over "
				        "nor joined; published as a door so it can be seen.\n",
				        sn->name, reac_rival_refusal(sn->hunt.arb.rival),
				        sn->hunt.arb.mac[0], sn->hunt.arb.mac[1], sn->hunt.arb.mac[2],
				        sn->hunt.arb.mac[3], sn->hunt.arb.mac[4], sn->hunt.arb.mac[5]);
			/* AND IT IS SERVED, as a door with no engine behind it. A refusal nobody
			 * can see is indistinguishable from a daemon that is not running: the
			 * 2026-09-09 rig proof refused correctly and the segment vanished from the
			 * console. reac_ifscan_heard is what queues the SERVE. */
			reac_ifscan_heard(&h->scan, sn->name, now);
			break;
		case REAC_HUNT_HUNTING:
		default:
			/* Heard, but nothing decides it — said once, because a state nobody can
			 * act on still has to be readable, and never a silent spinner. */
			if (!sn->undecided_said && !sn->hunt.pinned &&
			    reac_hunt_heard_anything(&sn->hunt) &&
			    now - sn->hunt.opened_ns >= REAC_HUNT_WINDOW_NS) {
				fprintf(stderr, "reac-pw: [%s] REAC heard but nothing decides the role "
				        "yet — no box announce and no master announce in %llu s; still "
				        "listening, transmitting nothing\n", sn->name,
				        (unsigned long long)(REAC_HUNT_WINDOW_NS / 1000000000ULL));
				sn->undecided_said = 1;
				/* AND IT GETS ITS DOOR ANYWAY (Q5, option C). REAC has been HEARD
				 * here — a vid seen on a trunk, or a frame on this wire — so the
				 * segment exists whatever we can make of it, and a segment with no
				 * node is one the console cannot render and the operator cannot set
				 * a role on. Served as a VACANT DOOR: one node carrying the
				 * segment's identity and `reac.master.state=none`, no engine of any
				 * kind. Its sniffer is KEPT (reac_watch_keep), so the first verdict
				 * that does decide this wire takes the door down and brings the real
				 * segment up in its place. */
				reac_ifscan_heard(&h->scan, sn->name, now);
			}
			break;
		}
	}
}

/* THE WIRE WAS OURS ONLY WHILE NOBODY ELSE CLAIMED IT, AND CLAIMS ARRIVE LATE.
 *
 * Every segment we took on a wire nobody pinned is watched here (its sniffer is the one
 * that was kept), and so is every REFUSED one. A wire we are driving can acquire a master
 * afterwards — a desk powered up second, a cable moved — and two masters on one segment is
 * the fault the seglock exists to make impossible between our own processes. It is no
 * better against a real desk, and the arbitration's law is not to fight: OBSERVE, then
 * act, and a foreign master is joined rather than out-shouted, a desk and a stagebox on M
 * alike (0.5.1: that wire is unpinned, and a box that wants the clock gets it).
 *
 * AND A YIELD IS NOT A ONE-WAY DOOR (0.5.4). The desk goes home, its sighting ages out of
 * the discovery table, and the segment must come back rather than sit slaved to a wire
 * nobody is driving — with the console's own boxes still on it. The sniffer is therefore
 * kept ACROSS the yield, not just up to it.
 *
 * Our own stream is not evidence: the sniffer's classifier is given this NIC's address
 * and every emitting role of ours sources from it (reac_mac.h), so the frames we are
 * putting on this very wire never reach the table.
 *
 * NOTHING LATCHES, IN ANY DIRECTION, AND NOTHING HERE DECIDES. reac_watch.h holds the
 * table — which of yield, retake, unrefuse or stand a fresh verdict means — because the
 * only thing that could exercise it in this file is a 70-second veth run that cannot
 * choose which route took the wire. This is the acting half: log the transition, then
 * drop and serve.
 *
 * A PIN IS NEVER OVERTURNED HERE. The operator answered for that wire; the one thing a
 * rival can do to it is the 0.5.1 refusal, decided by the segment's own engine below. */
static void hearing_yield(struct hearing *h, uint64_t now)
{
	for (int i = 0; i < REAC_IFSCAN_MAX; i++) {
		struct sniffer *sn = &h->sniff[i];
		if (!sn->name[0])
			continue;
		struct listener *L = hearing_listener(h, sn->name);
		if (!L)
			continue;   /* the segment went away; nothing to yield */
		struct reac_watch_in in = {
			.door       = L->cfg.door_only,
			.vacant     = L->cfg.door_vacant,
			/* A TAP IS NOT MASTERING ANYTHING. cfg.role holds one of the wire's two
			 * ends and a deferred tap leaves it at the default MASTER, which would
			 * read here as "we are driving this segment" — the one thing the role
			 * is defined by never doing. */
			.we_master  = !L->cfg.door_only && !L->cfg.tap &&
			              L->cfg.role == REAC_ROLE_MASTER,
			.pinned     = L->cfg.role_pinned,
			.verdict    = sn->hunt.verdict,
			.now_ns     = now,
			.opened_ns  = sn->hunt.opened_ns,
		};
		if (!in.door && !sn->watched)
			continue;   /* we never had this wire, or a pin answered for it */
		if (!reac_hunt_step(&sn->hunt, now))
			continue;   /* the verdict stands */
		in.verdict = sn->hunt.verdict;

		switch (reac_watch_decide(&in)) {
		case REAC_WATCH_STAND:
			continue;
		case REAC_WATCH_UNREFUSE: {
			/* THE REFUSAL ENDED. The box was switched to S, or unplugged, and its
			 * sighting aged out — so the wire the operator pinned is ours to drive
			 * after all. Down with the door, up with the segment, through the same
			 * seam a cold start uses. */
			fprintf(stderr, "reac-pw: [%s] the rival stopped mastering this wire — "
			        "the refusal is over; taking the segment as %s\n", sn->name,
			        reac_role_name(reac_hunt_role(&sn->hunt)));
			struct reac_hunt after = sn->hunt;
			hearing_drop(h, sn->name, "the refusal ended");
			hearing_serve(h, sn->name, &after);
			continue;
		}
		case REAC_WATCH_YIELD: {
			/* AND THE SENTENCE IS THE ACT HERE TOO. What the segment becomes is
			 * decided by the same predicate the serve reads, so a yield to a DESK
			 * says tap and a yield to a box on M says slave. */
			fprintf(stderr, "reac-pw: [%s] a %s masters this segment "
			        "(%02x:%02x:%02x:%02x:%02x:%02x) — we took this wire %s and it is "
			        "not ours to keep: yielding the master role and %s\n",
			        sn->name, reac_rival_kind_name(sn->hunt.arb.rival),
			        sn->hunt.arb.mac[0], sn->hunt.arb.mac[1], sn->hunt.arb.mac[2],
			        sn->hunt.arb.mac[3], sn->hunt.arb.mac[4], sn->hunt.arb.mac[5],
			        sn->driven_on_silence ? "because it was SILENT"
			                              : "because nothing was mastering it",
			        segment_defers_as_tap(segment_role_intent(sn->name), &sn->hunt)
			          ? "TAPPING it instead — nothing of ours goes back on this wire"
			          : "joining as SLAVE");
			/* Drop first, then serve: the two engines are exclusive (one AF_PACKET
			 * TX, one segment lock, one node pair) and the swap passes through a
			 * window in which nothing owns the segment — reac_role_swap.h says so
			 * and main() has always done it in this order. The sniffer is NOT
			 * closed, in either direction: the wire keeps being classified, so a
			 * desk that goes away again leaves a segment that can be re-decided
			 * rather than a latch. */
			struct reac_hunt verdict = sn->hunt;
			hearing_drop(h, sn->name, "yielding the master role to the wire's master");
			hearing_serve(h, sn->name, &verdict);
			continue;
		}
		case REAC_WATCH_RETAKE: {
			/* THE VENUE CASE, the other half. The desk that took this wire from us
			 * has stopped mastering it — powered off at the end of the night, a
			 * cable pulled — and its sighting has aged out of the discovery table,
			 * which is what "it is really gone" means everywhere else here. The
			 * boxes on this segment are still ours to drive, so we take it back. */
			fprintf(stderr, "reac-pw: [%s] the desk stopped mastering this wire — "
			        "taking the segment back as MASTER: probe, grant, establish\n",
			        sn->name);
			struct reac_hunt verdict = sn->hunt;
			hearing_drop(h, sn->name, "the wire's master went away — taking it back");
			hearing_serve(h, sn->name, &verdict);
			continue;
		}
		}
	}
}

/* The 200 ms poll's share: expire holds, apply whatever the table queued. A
 * listener whose capture socket lost its interface is a DROP here, not a
 * process exit — failure is isolated to its segment (§9). */
/* A PINNED MASTER THAT FINDS A BOX MASTERING ITS WIRE JOINS IT (operator, 2026-09-16: "we
 * set the daemons to enroll any box, master or slave").
 *
 * The pin says which end we want; a stagebox on M has already answered, and the daemon
 * settles it by taking the box's audio rather than by out-shouting it or serving nothing.
 * It REFUSED until 2026-09-16, and the rig measured what that cost: a pinned `enp131s0`
 * with an S-1608 on M published a door, moved no audio, and the operator read "not
 * detected". What the switch position still costs is the head-amp — a box on M has no mixer
 * behind it — and that is the console's to report beside a segment that works.
 *
 * WHY IT CANNOT BE DECIDED BEFORE THE ENGINE STARTS, which is the whole reason this lives
 * here and not in the hunt. A pin is served ON LINK with no frame waited for — a cold
 * stagebox in slave mode transmits nothing until a master announces to it, and requiring a
 * frame on a pinned wire is the 2026-09-08 outage. The sniffer's socket is opened in the
 * same 200 ms poll that takes the decision, so at that instant the table is empty by
 * construction; and a box on M announces its MASTER signature about once a second, so even
 * a listening window would have to be a whole announce cadence of added latency on EVERY
 * pinned wire, silent or not. Measured on the veth proof: the hunt-side verdict fired zero
 * times. So the pin drives, and its OWN engine — which classifies every frame on that wire
 * already — is what notices. The segment then comes down and is re-served as that box's
 * slave, through the one serve path.
 *
 * `reac_sink_node_rival_box` reports FOREIGN only while we are neither established nor
 * granting, so a segment that has actually enrolled a box is never taken away from it. */
static void hearing_join_box_master_on_pinned(struct hearing *h, uint64_t now)
{
	(void)now;
	for (int i = 0; i < h->n_slots; i++) {
		struct listener *L = &h->listeners[i];
		if (!L->opened || L->cfg.door_only || !L->sink)
			continue;
		if (!L->cfg.role_pinned || L->cfg.role != REAC_ROLE_MASTER)
			continue;   /* an unpinned wire reaches the same join through the hunt */
		uint8_t mac[6];
		unsigned channels = 0;
		if (!reac_sink_node_rival_box(L->sink, mac, &channels))
			continue;
		char name[IFNAMSIZ];
		snprintf(name, sizeof name, "%s", L->cfg.rxcfg.source ? L->cfg.rxcfg.source : "");
		if (!name[0])
			continue;
		fprintf(stderr, "reac-pw: [%s] %s [segment %s] role pins this segment MASTER and "
		        "%02x:%02x:%02x:%02x:%02x:%02x masters it at %u ch, a BOX width — "
		        "JOINING it as its slave at that width. The box's audio is served; its "
		        "head-amp is not reachable in master mode, so set its REAC Mode switch to "
		        "S and power-cycle it if you need the preamps.\n",
		        name, REAC_SEGCONF_FILE, name,
		        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], channels);
		/* The verdict is carried in the same shape the hunt would have handed over, so
		 * there is ONE serve path and the segment is configured by the same lines
		 * whichever side of its life the join was decided on. */
		struct reac_hunt joined;
		memset(&joined, 0, sizeof joined);
		joined.verdict = REAC_HUNT_SLAVE;
		joined.arb.state = REAC_SEGMENT_FOREIGN;
		joined.arb.rival = REAC_RIVAL_BOX;
		joined.arb.rival_channels = channels;
		joined.arb.have_mac = 1;
		memcpy(joined.arb.mac, mac, 6);
		hearing_drop(h, name, "a box masters this wire — joining it as its slave");
		hearing_serve(h, name, &joined);
	}
}

/* A SEGMENT THAT FOLLOWS NOBODY GOES BACK TO THE HUNT (#97).
 *
 * Measured on the rig 2026-09-13, reac-pw 1.0.1: VLAN 12 was pinned master, an S-1608 in
 * M mode was mastering it, and the daemon deferred to it — correctly. The box was set
 * back to S and power-cycled, so the foreign master left. Our slave went `ESTABLISHED ->
 * DROP -> FLOOD_ANNOUNCE` and STAYED there, courting a segment that no longer had a
 * master, while the cold box waited for one that was never going to announce. Neither
 * side could move for as long as it was left. `systemctl --user restart` brought the
 * segment up as master and enrolled the box in two seconds.
 *
 * THE EVIDENCE IS THE SEGMENT'S, NOT THE SNIFFER'S, AND THAT IS A MEASUREMENT AND NOT A
 * PREFERENCE. Keeping the hunt's sniffer alive on a joined segment (reac_watch_keep) is
 * the obvious shape and it is wrong: a box master's stream is mostly FILLER, which the
 * discovery peer lock deliberately refuses to treat as a sighting, so the wire looks
 * EMPTY to that table while an enrolment is in progress — with it enabled the daemon
 * retook the wire 6 s into a live box-master join and destroyed it. `reac_segment_heard`
 * asks the other question: are the master's frames still arriving AND DECODING through
 * this segment's own RX gate? That cannot be confused with our own courtship, because our
 * frames are ours and never reach it.
 *
 * BOUNDED, AND GENEROUSLY. The latch itself costs REAC_SEGMENT_HEARD_QUIET_TICKS (5 s)
 * before it clears — reac_disco's own "it is really gone" bar — and this waits the same
 * again on top, so nothing is re-decided until a master has been silent for ten seconds.
 * A REAC master fills every audio slot and cannot be present and silent for one of them,
 * let alone forty thousand.
 *
 * A PINNED RECORDER IS EXEMPT. `[segment X] role = slave` says BE THE BOX END HERE; a
 * wire with nobody on it does not change that, and re-serving it would reset the bounded
 * courtship's own backoff every ten seconds — which is the opposite of what that backoff
 * is for. Everything else re-hears the wire: a pin takes itself up again (the hunt serves
 * a pin on link), and `auto` takes whatever the wire now leaves open.
 *
 * AND SO IS A BOX, FOR THE SAME REASON ONE LEVEL STRONGER (2026-09-17 spec §5). `role =
 * box` says WE ARE THE STAGEBOX ON THIS WIRE, and a stagebox that only exists once a desk
 * is powered is not a stagebox: a quiet mixer is the normal state of a box waiting to be
 * plugged into a desk, not evidence about what this segment is. Re-deciding it every ten
 * seconds drops the declaration the row exists to make and rebuilds its published pair
 * around the gap — churn with nothing on the other end to notice it.
 *
 * IT RE-HEARS RATHER THAN RE-OPENS, through the same seam a role swap uses: drop, close
 * the sniffer, open a fresh one, ask for a serve. A segment re-opened on the OLD verdict
 * would slave to the master it just lost. */
#define REACPW_FOLLOWS_NOBODY_TICKS (2 * REAC_SEGMENT_HEARD_QUIET_TICKS)

static void hearing_reevaluate(struct hearing *h, uint64_t now)
{
	for (int i = 0; i < h->n_slots; i++) {
		struct listener *L = &h->listeners[i];
		if (!L->opened || L->cfg.door_only)
			continue;   /* a door follows nobody by construction */
		if (!L->cfg.tap && L->cfg.role != REAC_ROLE_SLAVE)
			continue;   /* only a segment that FOLLOWS can be left following nobody */
		if (L->cfg.role_intent == REAC_ROLE_INTENT_SLAVE ||
		    L->cfg.role_intent == REAC_ROLE_INTENT_BOX)
			continue;   /* the recorder, or the box, was asked for; an empty wire
			             * is its own case and never a re-classification */
		if (L->heard.heard) {
			L->follows_nobody_ticks = 0;
			continue;
		}
		if (++L->follows_nobody_ticks < REACPW_FOLLOWS_NOBODY_TICKS)
			continue;
		char name[IFNAMSIZ];
		snprintf(name, sizeof name, "%s", L->cfg.rxcfg.source ? L->cfg.rxcfg.source : "");
		if (!name[0])
			continue;
		fprintf(stderr, "reac-pw: [%s] the master this segment was following has been "
		        "silent for %d s — it is gone, and %s is not a state to sit in: "
		        "re-hearing the wire so the segment is CLASSIFIED afresh%s\n", name,
		        (int)((REACPW_FOLLOWS_NOBODY_TICKS + REAC_SEGMENT_HEARD_QUIET_TICKS) / 5),
		        L->cfg.tap ? "a tap with nothing to serve" : "courting nobody",
		        L->cfg.role_pinned ? " and the reac-pw.conf pin is taken up again" : "");
		hearing_drop(h, name, "the master it was following is gone — re-hearing the wire");
		/* The sniffer's hunt is stale for the same reason a role swap's is: it was
		 * decided against a master that is no longer there. A fresh one reads the pin
		 * as it now is and classifies the wire as it now is. */
		sniffer_close(h, name);
		sniffer_open_ex(h, name, 0);
		reac_ifscan_serve_failed(&h->scan, name, now);
	}
}

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
	hearing_hunt(h, now);
	hearing_join_box_master_on_pinned(h, now);
	hearing_reevaluate(h, now);
	hearing_yield(h, now);
	reac_ifscan_tick(&h->scan, now);
	hearing_apply(h);
	topo_apply(h, now);
}

static int hearing_start(struct hearing *h, struct pw_loop *loop, struct listener *slots,
                         int n_slots, int forced_rate)
{
	memset(h, 0, sizeof *h);
	h->loop = loop;
	h->listeners = slots;
	h->n_slots = n_slots;
	h->forced_rate = forced_rate;
	reac_topo_init(&h->topo);
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
	/* THE DECLARED SEGMENTS, BEFORE ANYTHING IS HEARD — that is the whole point. */
	declared_load(h);
	declared_ensure(h);
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
	/* THE CLEAN EXIT OF §4d, and it runs BEFORE the taps close so the releases it
	 * queues are still applied: every netdev we minted is removed, every netdev we
	 * adopted is left exactly as we found it. An unclean exit leaves ours behind, and
	 * the mint alias is what lets the next start re-own them instead of inheriting
	 * them as somebody else's for ever. */
	reac_topo_release_all(&h->topo);
	struct reac_topo_event ev;
	while (reac_topo_next(&h->topo, &ev))
		if (ev.verb == REAC_TOPO_RELEASE)
			topo_release(h, ev.parent, ev.vid, ev.minted);
	declared_release_all(h);
	for (int i = 0; i < REAC_TOPO_MAX_PARENTS; i++)
		if (h->tap[i].parent[0])
			topo_unwatch_iface(h, h->tap[i].parent);
	for (int i = 0; i < REAC_IFSCAN_MAX; i++)
		if (h->sniff[i].name[0])
			sniffer_close(h, h->sniff[i].name);
	if (h->nl_io)
		pw_loop_destroy_source(h->loop, h->nl_io);
	reac_ifscan_close(&h->scan);
	h->enabled = 0;
}

/* ---- THE ROSTER TICK (spec 2026-09-16-segments-and-roles-are-autodetected.md, amendment
 * 2026-09-16 third, §B) ------------------------------------------------------------------
 *
 * READ, NEVER WRITTEN. Every field below is derived, on the tick, from the table that
 * already owns it: the conf for `ignored`, the listener's own engine for tap / slave /
 * refused, the master's recognized box for `established`, and the sniffer table for the
 * segments this daemon is hearing and has decided nothing about. A roster FIELD anywhere
 * would be a second answer to a question that already has one, and the second answer is
 * the one that goes stale.
 *
 * WHY THIS NODE AND NOT THE SEGMENT'S OWN. The amendment one level up removed the
 * zero-port per-segment door, and said in its own words what that cost: the console's
 * roster is a graph scan, so a segment with no node has no row at all — not even one
 * saying `probing`. That is the row. */
struct roster_ctx { struct listener *listeners; int n; struct pw_loop *loop; };

/* The interface name this listener serves. rxcfg.source is what every other reader here
 * uses; iface_buf is where a HEARD segment's name lives and is what rxcfg.source points
 * at, so the fallbacks are the same string by two routes and never a second identity. */
static const char *roster_seg_name(const struct listener *L)
{
	if (L->cfg.rxcfg.source && *L->cfg.rxcfg.source)
		return L->cfg.rxcfg.source;
	if (L->cfg.iface_buf[0])
		return L->cfg.iface_buf;
	return L->cfg.inst_name;
}

/* `conf:<file>` when a file pinned this segment's role, `autodetected` otherwise — the
 * FILE and not the layer, because with a drop-in directory behind the conf "the config
 * said so" names nothing an operator can open. */
static void roster_source(const char *seg, char *out, size_t len)
{
	const char *f = reac_segconf_role_file(&g_segconf, seg);
	if (!f)
		f = reac_segconf_ignore_file(&g_segconf, seg);
	if (f)
		snprintf(out, len, "conf:%s", f);
	else
		snprintf(out, len, "autodetected");
}

static void roster_add_listener(struct listener *L, int *dup)
{
	const char *seg = roster_seg_name(L);
	if (!seg || !*seg)
		return;
	enum reac_roster_state st;
	const char *role;
	const struct reac_box_model *bm = L->sink ? reac_sink_node_recognized_box(L->sink) : NULL;
	if (L->cfg.tap) {
		st = REAC_ROSTER_TAP;
		role = "tap";
	} else if (L->cfg.door_only && !L->cfg.door_vacant) {
		/* A REFUSAL IS A STATE, not an absence: a wire pinned master with a box
		 * mastering it is the one case the daemon will not serve, and it is exactly
		 * the case an operator must be able to SEE from the console. */
		st = REAC_ROSTER_REFUSED;
		role = reac_role_name(L->cfg.role);
	} else if (L->cfg.box_model) {
		/* A BOX IS THE SLAVE END OF THE PAIRING AND A DIFFERENT ROLE TO READ. The
		 * state stays the slave one — that is what the engine is doing — and the ROLE
		 * is what the operator asked for, which is the fact a console renders. */
		st = REAC_ROSTER_SLAVE;
		role = reac_role_intent_name(REAC_ROLE_INTENT_BOX);
	} else if (L->cfg.role == REAC_ROLE_SLAVE || L->cfg.join_box_master) {
		st = REAC_ROSTER_SLAVE;
		role = "slave";
	} else {
		st = bm ? REAC_ROSTER_ESTABLISHED : REAC_ROSTER_PROBING;
		role = "master";
	}
	/* THE MODEL IS WHAT IS RECOGNISED, or what the operator PINNED — a `--box` pin is
	 * the one declaration the no-node ruling keeps, so the roster must carry it too.
	 * `none` otherwise, said out loud: an absent box must SAY absent. */
	/* THE TOKEN, because `reac.box-model` on the segment's own node already publishes
	 * `bm->token` and `none` — the console keys off that vocabulary, and a roster that
	 * spelled the same box a second way would be a second vocabulary for one fact. The
	 * display string also carries the width, which `.width` already owns. */
	const char *model = L->cfg.box_model ? L->cfg.box_model->token
	                   : (bm ? bm->token
	                         : (L->cfg.pin_model ? L->cfg.pin_model->token : NULL));
	/* THE WIDTH IS THE PAIR'S, and the pair exists only where a box does. A slave's
	 * capture is the width of the stream it receives, which the hunt already carried
	 * into wire_channels; nothing else invents a number. */
	int in = 0, out = 0;
	if (L->cfg.box_model) {
		/* WHAT WE DECLARE, not what the wire carries: the pair is published from the
		 * row at start, so the roster reads the row too. */
		in = L->cfg.box_model->in_ch;
		out = L->cfg.box_model->out_ch;
	} else if (bm) {
		in = bm->in_ch;
		out = bm->out_ch;
	} else if (L->cfg.pin_model) {
		in = L->cfg.pin_model->in_ch;
		out = L->cfg.pin_model->out_ch;
	} else if (st == REAC_ROSTER_SLAVE || st == REAC_ROSTER_TAP) {
		in = (int)L->cfg.wire_channels;
	}
	char src[112];
	roster_source(seg, src, sizeof src);
	if (reac_roster_add(&g_roster, seg, st, model, role, src, in, out) != 0)
		(*dup)++;
}

static void roster_collect(struct roster_ctx *rc)
{
	int dup = 0;
	reac_roster_begin(&g_roster);
	/* 1. THE SEGMENTS THE CONF SWITCHED OFF. They are never sniffed and never served, so
	 *    no other table holds them — and a segment the operator silenced is precisely
	 *    one a console must be able to explain, rather than one that is merely missing. */
	for (int i = 0; i < g_segconf.n; i++) {
		if (!g_segconf.seg[i].ignore)
			continue;
		char src[112];
		roster_source(g_segconf.seg[i].name, src, sizeof src);
		enum reac_role_intent want = REAC_ROLE_INTENT_AUTO;
		reac_segconf_role(&g_segconf, g_segconf.seg[i].name, &want);
		if (reac_roster_add(&g_roster, g_segconf.seg[i].name, REAC_ROSTER_IGNORED,
		                    NULL, reac_role_intent_name(want), src, 0, 0) != 0)
			dup++;
	}
	/* 2. EVERY SEGMENT WITH AN ENGINE. */
	for (int i = 0; i < rc->n; i++)
		if (rc->listeners[i].opened)
			roster_add_listener(&rc->listeners[i], &dup);
	/* 3. AND EVERY WIRE WE ARE LISTENING TO THAT HAS NOT DECIDED ANYTHING YET — the
	 *    `probing` row this whole node exists for. A sniffer whose segment is served is
	 *    already row 2's; hearing_listener is the same lookup that owns that fact. */
	for (int i = 0; g_hear.enabled && i < REAC_IFSCAN_MAX; i++) {
		const char *nm = g_hear.sniff[i].name;
		if (!nm[0] || hearing_listener(&g_hear, nm))
			continue;
		char src[112];
		roster_source(nm, src, sizeof src);
		enum reac_role_intent want = segment_role_intent(nm);
		if (reac_roster_add(&g_roster, nm, REAC_ROSTER_PROBING, NULL,
		                    reac_role_intent_name(want), src, 0, 0) != 0)
			dup++;
	}
	if (dup) {
		/* Two tables claiming one wire, or more segments than the roster holds.
		 * Neither is survivable in silence: a console would read a rig that is not
		 * there. Said once — a tick repeats 120 times a minute. */
		static int said;
		if (!said++)
			fprintf(stderr, "reac-pw: the segment roster refused %d entr%s this tick "
			        "(bound %d, or one segment claimed twice) — the roster node is "
			        "INCOMPLETE and this is said once\n",
			        dup, dup == 1 ? "y" : "ies", REAC_ROSTER_MAX);
	}
}

static void on_roster_timer(void *data, uint64_t expirations)
{
	(void)expirations;
	struct roster_ctx *rc = data;
	if (!g_roster_node) {
		/* LAZY AND RETRIED, because a daemon can be up before PipeWire is. Every
		 * tenth tick so a graph-less run costs one connect attempt every 5 s, and the
		 * refusal is said ONCE — a line per tick would bury the journal that is an
		 * empty segment's other home. */
		static int ticks;
		if (ticks++ % 10)
			return;
		g_roster_node = reac_roster_node_new(rc->loop);
		if (!g_roster_node) {
			static int said;
			if (!said++)
				reac_code_emit(stderr, "reac-pw", RC_E_ROSTER_NODE,
				    "the segment roster node could not be created (no PipeWire?) "
				    "— the segments are in this journal and nowhere else until it "
				    "can be; retrying\n");
			return;
		}
	}
	/* THE ANNOUNCEMENT WAITS FOR THE ID, because the operator's next command is
	 * `pw-cli info <id>`. The export is a round trip, so the id is SPA_ID_INVALID for a
	 * tick or two after the connect. */
	if (!g_roster_said) {
		uint32_t id = reac_roster_node_id(g_roster_node);
		if (id != SPA_ID_INVALID) {
			g_roster_said = 1;
			fprintf(stderr, "reac-pw: the segment roster is on the graph: node "
			        "`reac-pw` id %u, no ports, reac.roster=1 — every segment this "
			        "daemon runs, probing ones included. Read it with "
			        "`pw-cli info %u`\n", id, id);
		}
	}
	roster_collect(rc);
	/* A GROUP THAT LEAVES TAKES THE NODE WITH IT (#106, spec amendment 2026-09-20 §b).
	 * A PipeWire node cannot be told to DROP a property — the NULL-value removal is
	 * applied to the client's own dict and what crosses is a merge of what is left — so
	 * `reac.roster.n` fell to 0 while four stale groups stayed on the graph, one of them
	 * `established 32/8` on a wire with no carrier. The only honest removal is a fresh
	 * node: destroy, rebuild, forget what was published, and let the delta below emit the
	 * WHOLE roster as sets. Rare by construction — every other tick, including every
	 * state, width, model and provenance change, is still a property update on a node
	 * whose id does not move. */
	if (reac_roster_shrank(&g_roster)) {
		fprintf(stderr, "reac-pw: a segment left the roster — rebuilding the `reac-pw` "
		        "roster node, because a PipeWire node cannot be told to drop a property "
		        "and a group left behind is a console reading a box that is not there\n");
		reac_roster_node_destroy(g_roster_node);
		g_roster_node = reac_roster_node_new(rc->loop);
		reac_roster_forget(&g_roster);
		g_roster_said = 0;      /* a new node has a new id, and the operator reads it */
		if (!g_roster_node) {
			reac_code_emit(stderr, "reac-pw", RC_E_ROSTER_NODE,
			    "the roster node could not be rebuilt after a segment left it (no "
			    "PipeWire?) — there is NO roster on the graph until it can be; "
			    "retrying\n");
			return;
		}
	}
	struct reac_roster_kv kv[REAC_ROSTER_KV_MAX];
	int n = reac_roster_delta(&g_roster, kv, REAC_ROSTER_KV_MAX);
	if (n < 0) {
		static int said;
		if (!said++)
			fprintf(stderr, "reac-pw: the roster delta did not fit its own bound and "
			        "was NOT published — the roster node is STALE (said once)\n");
		return;
	}
	if (n == 0)
		return;   /* nothing moved: the whole point — no property storm, no node churn */
	/* COMMIT ONLY WHAT WAS PUBLISHED. A refused delta that was committed anyway would
	 * make the next tick believe the node carries what it does not — the header says so
	 * and it is the one way this module can lie. */
	if (reac_roster_node_publish(g_roster_node, kv, n) != 0)
		return;
	reac_roster_commit(&g_roster);
}

struct rate_reopen_ctx { struct listener *listeners; int n; struct pw_loop *loop; };

/* Set when a segment's capture socket lost its interface (see reac_rx.h's
 * bound_ifindex). main() turns it into a non-zero exit so the service manager
 * restarts us; read only on the loop thread after pw_main_loop_run returns. */
static int g_iface_lost;

/* A ROLE CHANGE ON A HEARD SEGMENT RE-ASKS THE WIRE, IT DOES NOT GUESS (0.5.6-8).
 *
 * `listener_reopen_at_role` swaps the engine in place and carries the listener's
 * configuration across — which is right for a `--live` segment, where nothing classified it
 * in the first place. On a HEARD segment it is wrong, and the rig showed how: a wire pinned
 * MASTER with a stagebox on M was published as the 0.5.1 refusal door; the pin was changed
 * to `auto` through the console, and the in-place swap took the segment into the DESK-slave
 * engine — "rx stream = master downstream (40 ch)", role_reestablish_pending — because
 * `join_box_master` and `wire_channels` are the HUNT's verdict and the swap never re-runs it.
 * A service restart took the right path, which is the tell: the difference was the
 * classification, not the role.
 *
 * So a heard segment is DROPPED instead. Its sniffer re-hears the wire, the hunt classifies
 * it afresh with the new pin in hand, and `hearing_serve` opens it through the same seam a
 * cold start uses — the one path that has ever been right about what is on a wire. Returns 1
 * when it took the segment down, 0 when this is not a heard segment and the caller should do
 * the in-place swap.
 *
 * THE ROLE ASSERTION IS NOT LOST. It was written to the conf by whoever asked for it, which
 * is where `listener_cfg_from_conf` reads it on the way back up. */
static int listener_reopen_role_reclassify(struct listener *L, struct pw_loop *loop,
                                           enum reac_role role);

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
		/* A BOX-MASTER JOIN HAS A PLAYBACK NODE AND STILL ANSWERS FROM ITS DOOR
		 * (0.5.6). That node is a graph door with no pacer behind it — it publishes
		 * nothing about the segment and has no drains to take — so this segment is
		 * polled exactly as any other slave: the capture node carries the write door
		 * and the answer. */
		if (L->cfg.tap) {
			/* No write door: a tap's role is asserted by the console into
			 * reac-pw.env and taken at open, and there is no engine here to
			 * swap. What moves between ticks is whether the master is still
			 * being heard, so the answer is re-derived and republished. */
			listener_publish_tap(L);
			continue;
		}
		if (!L->sink || L->cfg.join_box_master) {
			int back = reac_source_node_take_reopen_role(L->src);
			if (back >= 0) {
				if (!listener_reopen_role_reclassify(L, c->loop,
				                                     (enum reac_role)back))
					listener_reopen_at_role(L, c->loop, (enum reac_role)back);
				continue;   /* the capture node was rebuilt; nothing more this tick */
			}
			listener_publish_segment(L);
			continue;
		}
		int role = reac_sink_node_take_reopen_role(L->sink);
		if (role >= 0) {
			if (!listener_reopen_role_reclassify(L, c->loop, (enum reac_role)role))
				listener_reopen_at_role(L, c->loop, (enum reac_role)role);
			continue;   /* the master sink is gone after a swap to slave; nothing more this tick */
		}
		int hz = reac_sink_node_take_reopen_rate(L->sink);
		if (hz > 0)
			listener_reopen_at_rate(L, c->loop, hz);
	}
	hearing_poll(&g_hear);
}


/* WHAT WE DETECTED AND WHAT THE FILE OVERRODE, said at start, before a socket is opened
 * (spec §4). Absent is the normal case and is printed as such: "absent" and "empty" must
 * not read alike, because one of them means the operator's file is not where they think
 * it is. */
static void segconf_announce(void)
{
	if (g_segconf.n_files == 0) {
		fprintf(stderr, "reac-pw: no %s and no %s/ — every segment autodetects: roles "
		        "come from the wire (auto) and segments from the host's interfaces. "
		        "Looked at %s\n",
		        REAC_SEGCONF_FILE, REAC_SEGCONF_DIRD, g_segconf.path);
	} else {
		/* THE FILES, IN THE ORDER THEY WERE OBEYED. With a drop-in directory behind
		 * the conf, "the config said so" names nothing an operator can open — and an
		 * order they cannot read back is an order they will get wrong (spec amendment
		 * 2026-09-16 third, §C). */
		fprintf(stderr, "reac-pw: %d configuration file(s), read in this order, LAST "
		        "WINS per key:\n", g_segconf.n_files);
		for (int i = 0; i < g_segconf.n_files; i++)
			fprintf(stderr, "reac-pw:   %d. %s/%s\n", i + 1, g_segconf.base,
			        g_segconf.file[i]);
		fprintf(stderr, "reac-pw: %d segment(s) overridden\n", g_segconf.n);
		for (int i = 0; i < g_segconf.n; i++) {
			const struct reac_segconf_seg *sg = &g_segconf.seg[i];
			/* AND WHICH FILE SET EACH KEY. This is the whole safety of last-wins:
			 * an override nobody can trace back to a file is the 2026-09-16 fault
			 * with one more file in it. */
			fprintf(stderr, "reac-pw:   [segment %s]%s%s%s%s%s%s%s%s\n", sg->name,
			        sg->role_set ? " role=" : "",
			        sg->role_set ? reac_role_intent_name(sg->role) : "",
			        sg->role_set ? " (" : "",
			        sg->role_set ? sg->role_file : "",
			        sg->role_set ? ")" : "",
			        sg->ignore ? " ignore (" : "",
			        sg->ignore ? sg->ignore_file : "",
			        sg->ignore ? ")" : "");
		}
	}
	for (int i = 0; i < g_segconf.n_refusals; i++)
		fprintf(stderr, "reac-pw: REFUSED %s\n", g_segconf.refusal[i]);
	if (g_segconf.refused > (unsigned)g_segconf.n_refusals)
		fprintf(stderr, "reac-pw: %u line(s) were refused in all; the first %d are "
		        "above, each naming its own file\n",
		        g_segconf.refused, g_segconf.n_refusals);
	/* AND THE HOST-WIDE KEY THAT NO LONGER DOES ANYTHING. The per-segment ones are named
	 * as each interface is met (segment_say_env_role_retired); this is the bare one, which
	 * belongs to no interface and would otherwise never be mentioned at all. */
	char v[256];
	enum reac_conf_layer l = reac_conf_lookup("REAC_ROLE", NULL, NULL, v, sizeof v);
	if (l != REAC_CONF_NONE)
		fprintf(stderr, "reac-pw: REAC_ROLE='%s' in %s is IGNORED: there is no host-wide "
		        "role — a role is a fact about ONE wire, and the only thing that can "
		        "state one is %s [segment <name>] role= (spec 2026-09-16)\n",
		        v, reac_conf_layer_name(l), REAC_SEGCONF_FILE);
}

/* REACPW_CLOCK_REF, resolved ONCE into a STATIC buffer — the same process-lifetime
 * storage guarantee a bare getenv() gave, now reached through reac_knobs_resolve so
 * argv/conf/env all work (2026-09-17, the fix that removed the last env-only knob
 * exception; see the comment at its two call sites). NULL when unset, matching
 * getenv()'s own NULL-on-unset contract. */
static const char *resolve_clock_ref(void)
{
	static char buf[256];
	static int done;
	static const char *cached;
	if (!done) {
		done = 1;
		if (reac_knobs_resolve("REACPW_CLOCK_REF", buf, sizeof buf) != REAC_CONF_NONE)
			cached = buf;
	}
	return cached;
}

/* THE OTHER SIDE OF DISCOVERY AND PUBLISH: libreac reads no environment of its own
 * (libreac's docs/design/specs/2026-09-17-tunables-api-and-shared-refusal-codes.md)
 * — every REACPW_* / REAC_* knob that used to be a bare getenv INSIDE reac_master.c,
 * reac_pacer.c, reac_ifscan.c or reac_rx.c is now a field this daemon resolves
 * through g_reac_knobs (so it is announced above, exactly like every other knob)
 * and PUSHES into the library through reac_*_tunables_set(), once, before the
 * transport starts. Call after reac_knobs_announce() and before hearing_start(). */
static void push_libreac_tunables(void)
{
	char v[256];

	struct reac_master_tunables mt = REAC_MASTER_TUNABLES_DEFAULT;
	mt.grant_on_declare = reac_knobs_resolve_flag("REACPW_GRANT_ON_DECLARE", 1);
	if (reac_knobs_resolve("REACPW_GRANT_DWELL_MS", v, sizeof v) != REAC_CONF_NONE)
		mt.grant_dwell_ms = strtol(v, NULL, 10);
	if (reac_knobs_resolve("REACPW_GRANT_DWELL_S", v, sizeof v) != REAC_CONF_NONE)
		mt.grant_dwell_s = strtol(v, NULL, 10);
	mt.no_enroll = reac_knobs_resolve_flag("REACPW_NO_ENROLL", 0);
	mt.est_scene = reac_knobs_resolve_flag("REACPW_EST_SCENE", 0);
	reac_master_tunables_set(&mt);

	struct reac_pacer_tunables pt = REAC_PACER_TUNABLES_DEFAULT;
	if (reac_knobs_resolve("REACPW_GUARD_FLOOR_FRAMES", v, sizeof v) != REAC_CONF_NONE) {
		char *end = NULL;
		unsigned long f = strtoul(v, &end, 10);
		if (end && *end == '\0')
			pt.guard_floor_frames = (unsigned int)f;
	}
	pt.no_headamp = reac_knobs_resolve_flag("REACPW_NO_HEADAMP", 0);
	reac_pacer_tunables_set(&pt);

	struct reac_transport_tunables tt = REAC_TRANSPORT_TUNABLES_DEFAULT;
	if (reac_knobs_resolve("REAC_IFACES_ALLOW_WIRELESS", v, sizeof v) != REAC_CONF_NONE)
		tt.allow_wireless = v;   /* reac_transport_tunables_set copies it */
	tt.debug = reac_knobs_resolve_flag("REAC_DEBUG", 0);
	reac_transport_tunables_set(&tt);
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

	/* THE COMMAND LINE, HIGHEST PRECEDENCE (operator ruling, 2026-09-17): a
	 * generic, repeatable `--set KEY=VALUE` resolves against g_reac_knobs by
	 * name — the same table --help lists and reac_knobs_announce walks — rather
	 * than adding a bespoke flag per knob. An unknown key is refused here, before
	 * any capability or segment is touched: a typo in an override must not start
	 * the daemon at a default the operator did not ask for. Parsed before
	 * reac_knobs_announce() so a --set value is what gets announced. */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--set") != 0)
			continue;
		if (i + 1 >= argc) {
			reac_code_emit(stderr, "reac-pw", RC_E_UNKNOWN_KNOB,
			                "--set needs KEY=VALUE\n");
			return 2;
		}
		char *eq = strchr(argv[i + 1], '=');
		if (!eq) {
			reac_code_emit(stderr, "reac-pw", RC_E_UNKNOWN_KNOB,
			                "--set %s: not KEY=VALUE\n", argv[i + 1]);
			return 2;
		}
		*eq = '\0';   /* argv[] is writable; split in place, once */
		const char *key = argv[i + 1];
		const char *value = eq + 1;
		if (!reac_knobs_set_argv(key, value)) {
			reac_code_emit(stderr, "reac-pw", RC_E_UNKNOWN_KNOB,
			                "--set %s: no such knob (see --help)\n", key);
			return 2;
		}
		i++;   /* consumed the KEY=VALUE argument too */
	}

	/* Before ANY capability is even read: uid 0 already carries every one of
	 * them, so it must be refused first or the check right after would pass
	 * silently over the one identity that must never start this daemon. */
	refuse_if_root();

	/* Before anything is opened, per §4e: a missing capability must arrive as a
	 * sentence, not as a daemon that runs deaf. */
	capability_preflight();

	/* DISCOVERY AND PUBLISH (2026-09-17 ruling, §1): every env/conf override in
	 * force, named, before any of them is acted on. */
	reac_knobs_announce(stderr);

	/* "FIX INSTALLATION": an expert who has set nothing yet gets ONE line saying
	 * where overrides go, rather than silence that reads as "there is nothing to
	 * configure". Autodetect is the default in every other sense; this is the
	 * discoverable path to the expert one (packaging/reac-pw.conf.example,
	 * reac-pw.env.example — both %doc in the RPM). */
	{
		const char *home = getenv("HOME");
		char path[512];
		struct stat st;
		int conf_present = 0;
		if (home && *home) {
			snprintf(path, sizeof path, "%s/.config/reac-pw/reac-pw.conf", home);
			conf_present = (stat(path, &st) == 0);
		}
		if (!conf_present)
			reac_code_emit(stderr, "reac-pw", RC_S_NO_OVERRIDES,
			                "no ~/.config/reac-pw/reac-pw.conf — every segment "
			                "autodetects. Expert overrides: reac-pw.conf, "
			                "reac-pw.conf.d/, reac-pw.env, REACPW_*/REAC_* env, or "
			                "--set KEY=VALUE (see packaging/reac-pw.conf.example, "
			                "reac-pw.env.example, --help)\n");
	}

	/* THE OTHER SIDE OF DISCOVERY AND PUBLISH: every knob libreac used to read
	 * itself is resolved here (same table, same cli>env>conf precedence as
	 * everything above) and pushed in before the transport starts. */
	push_libreac_tunables();

	/* THE ONE OVERRIDE, READ BEFORE ANY DECISION IS TAKEN. Every question about a
	 * segment's role or whether to touch it at all is asked of this, so it has to be
	 * loaded before the first listener_cfg_from_conf and before hearing_start. */
	reac_segconf_load(&g_segconf, NULL);
	segconf_announce();

	/* ---- the CLI template: byte-identical to every invocation before this one.
	 * These are the flags/variables main() always had; they describe ONE
	 * segment (the first --live, or --pcap) and nothing else. Auto-spine's
	 * extra segments never read them — see listener_cfg_from_conf. */
	enum reac_conf_layer rate_layer = REAC_CONF_NONE;
	struct reac_rx_cfg rxcfg = { .kind = REAC_RX_PCAP, .source = NULL, .forced_rate = 0,
	                             .pcap_realtime = 1 };
	const char *tx_if = NULL;
	enum reac_role role = REAC_ROLE_MASTER;   /* default master: preserves current behaviour */
	/* `--role tap` is not a value of `role` above — a tap presents no end of the
	 * pairing — so it rides beside it, exactly as listener_cfg.tap does. */
	int role_tap = 0;
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
		reac_mixer_profile_by_name("m200");   /* master: which desk name we log as */

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
			if (!strcmp(argv[++i], "tap")) {
				role_tap = 1;
			} else if (reac_role_parse(argv[i], &role) != 0) {
				fprintf(stderr, "reac-pw: unknown --role '%s' (master|slave|tap)\n",
				        argv[i]);
				return 2;
			} else {
				role_tap = 0;
			}
		} else if (!strcmp(argv[i], "--mixer") && i + 1 < argc) {
			/* Master role: which desk name reac-pw logs as. Does not set the
			 * wire's pace-code byte; grants are box-defined, so a box locks to
			 * any profile. */
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
		} else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
			/* ALREADY RESOLVED, ABOVE. The knob pre-pass reads every --set before
			 * a capability is touched, so all this arm owes is to step over the
			 * KEY=VALUE it consumed. Without it the flag loop fell through to
			 * `usage()` and the daemon EXITED 2 on the very flag --help documents:
			 * `--set` was proven by a unit test that calls reac_knobs_set_argv()
			 * and never ran the binary, so it had never once worked on a command
			 * line (found 2026-09-20, by the first test that passed one). */
			i++;
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
		c->tap = role_tap;
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
				c->tap = role_tap;
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

	/* THE ROSTER TICK. 500 ms: it walks at most 32 segments and publishes only what
	 * moved, and the thing it answers — "which wires is this daemon on, and what is
	 * each one doing" — changes on a human's timescale, not a frame's. */
	struct roster_ctx roctx = { listeners, n_listeners, loop };
	struct spa_source *roster_timer = pw_loop_add_timer(loop, on_roster_timer, &roctx);
	if (roster_timer) {
		struct timespec rfirst = { 0, 500 * 1000000L };
		struct timespec rint = { 0, 500 * 1000000L };
		pw_loop_update_timer(loop, roster_timer, &rfirst, &rint, false);
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
	reac_roster_node_destroy(g_roster_node);
	g_roster_node = NULL;
	for (int i = 0; i < n_listeners; i++)
		if (listeners[i].opened)
			listener_close(&listeners[i], loop);

	pw_main_loop_destroy(g_loop);
	pw_deinit();
	/* Non-zero so a Restart=always unit brings us back on the live interface;
	 * a clean SIGTERM shutdown still returns 0. */
	return g_iface_lost ? 1 : 0;
}
