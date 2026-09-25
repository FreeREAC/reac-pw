// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* THE AUTO-ROLE DETECTION, AGAINST THE BYTES A REAL WIRE CARRIED
 * (docs/design/specs/2026-09-16-auto-role-per-segment.md §1).
 *
 * tests/test_reac_hunt.c drives the same four outcomes from the libreac BUILDERS, which
 * is the right unit test and proves nothing about a box we have never decoded. This one
 * replays reac-captures pcaps through reac_hunt and requires the verdict, the rival kind,
 * the rival's WIDTH and the rival's MAC — the four facts a segment is sized and named
 * from — to come out of frames a Roland device actually sent.
 *
 * THE LIVE CASE IT REPRODUCES, 2026-09-16. An S-1608 with its REAC Mode switch on M, on
 * a segment the console's conf pinned `REAC_ROLE_<seg>=master`. The daemon refused the
 * wire (`rival-master-box`) and published a door with no audio; the operator saw "not
 * detected". The two `box-master-*` arms are that wire and its fix, from the same
 * capture: PINNED master refuses, UNPINNED (`auto`) slave-joins the box at its own 16
 * channels. The refusal is correct and the pin was the defect — see the spec's §4 and
 * openmixer's arbitration §8, ninth amendment, for the console half.
 *
 * THE SNAPLEN TRAP, AND WHY THIS PADS. Three of these captures ran at snaplen 512, and
 * the GEOMETRY — which is the whole classification — is read from the frame LENGTH. A
 * caplen buffer would make every frame read as no legal `52 + 36n` width, i.e. UNKNOWN,
 * i.e. refused: the test would go green on the wrong reason. Every pcap record carries
 * origlen beside caplen, so each frame is zero-padded back to its original length (minus
 * the 4 bytes pcap_source already stripped with the 802.1Q tag). The control block lives
 * in [18:50] and is inside every capture's snaplen, so the checksum still verifies —
 * `cksum_bad` is asserted to be zero, which is what proves the padding did not corrupt
 * the evidence rather than merely not crashing.
 *
 * A PROBE THAT CANNOT REPORT PRESENCE VOIDS EVERY ABSENCE IT REPORTS, so every arm
 * asserts it SAW something (frames, sightings, a table) before any arm is allowed to
 * assert that something is not there — which is exactly what the SP arm does.
 *
 * DEV-ONLY, like libreac's own corpus gate: the capture set is not in this repo. With no
 * corpus this exits 77 (meson SKIP) and says so on stderr. A silently skipped proof is
 * decoration.
 */
#include <reac/reac_hunt.h>

#include <reac/reac.h>
#include <reac/reac_arbitration.h>
#include <reac/reac_ctrlblk.h>
#include <reac/pcap_source.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* Our NIC: a MAC that appears in none of these captures, so nothing here is our echo. */
static const uint8_t OURS[6] = { 0x34, 0x5a, 0x60, 0x9f, 0x9e, 0xbe };
/* The two devices the arms below name, read off the captures themselves. */
static const uint8_t S1608_ON_M[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x3b };
static const uint8_t M200[6]       = { 0x00, 0x40, 0xab, 0xc9, 0xcc, 0x03 };

/* Bounded: these files run to 3 GB and the verdict is decided in the first seconds. */
#define REPLAY_MAX 200000

struct replay {
	unsigned long frames;      /* pcap records read                              */
	unsigned long sightings;   /* frames reac_hunt accepted as evidence          */
	unsigned long padded;      /* frames restored from origlen (the snaplen trap)*/
	unsigned long cksum_bad;   /* control frames whose checksum did NOT verify   */
	unsigned long announces;   /* cfea master announces — the presence control   */
	unsigned long splits;      /* 0xceea SPLIT_ANNOUNCE — never yet captured     */
	struct reac_hunt h;
};

/* Replay `path` through a fresh hunt. `pin` is a reac_role to pin, or -1 for `auto`.
 * Returns 0, or -1 when the file cannot be opened. */
static int replay(const char *path, int pin, struct replay *r)
{
	struct pcap_source ps;
	if (pcap_source_open(&ps, path) != 0)
		return -1;

	memset(r, 0, sizeof *r);
	reac_hunt_init(&r->h, OURS, 0);
	if (pin >= 0)
		reac_hunt_pin(&r->h, (enum reac_role)pin);

	uint8_t buf[2048];
	uint64_t t0 = 0, now = 0, ts = 0;
	long n;
	while ((n = pcap_source_next(&ps, buf, sizeof buf, &ts)) > 0) {
		if (t0 == 0)
			t0 = ts;
		now = (ts - t0) * 1000ULL;      /* pcap is microseconds; the hunt wants ns */

		size_t len = (size_t)n;
		uint32_t orig = ps.last_orig_len - (ps.last_vlan_tagged ? 4u : 0u);
		if (orig > (uint32_t)n && orig <= sizeof buf) {
			memset(buf + n, 0, orig - (uint32_t)n);
			len = orig;
			r->padded++;
		}
		r->frames++;

		struct reac_ctrl_parsed p;
		enum reac_ctrl_kind k = reac_ctrl_parse(buf, len, &p);
		if (k == REAC_CTRL_MASTER_ANNOUNCE)
			r->announces++;
		if (k == REAC_CTRL_SPLIT_ANNOUNCE)
			r->splits++;
		if (k != REAC_CTRL_NONE && k != REAC_CTRL_FILLER &&
		    reac_ctrl_checksum_verify(buf) != 0)
			r->cksum_bad++;

		if (reac_hunt_observe(&r->h, buf, len, now, NULL) >= 0)
			r->sightings++;
		if ((r->frames % 500) == 0)
			reac_hunt_step(&r->h, now);
		if (r->frames >= REPLAY_MAX)
			break;
	}
	/* The last step is taken at the LAST FRAME'S time, never at a round number past it:
	 * reac_hunt_step AGES the table first, so stepping at a clock the capture never
	 * reached stales every peer and reads back as a vacant wire. Off by that alone, all
	 * four arms below answer `hunting` and the test would have measured its own clock. */
	reac_hunt_step(&r->h, now);
	pcap_source_close(&ps);
	return 0;
}

/* The presence control every arm runs before it is allowed to assert an absence. */
static void saw_a_wire(const struct replay *r, const char *what)
{
	if (r->frames == 0 || r->sightings == 0 || r->h.table.n == 0)
		fprintf(stderr, "FAIL: %s — the probe saw nothing (frames=%lu sightings=%lu peers=%d)\n",
		        what, r->frames, r->sightings, r->h.table.n);
	CHK(r->frames > 0);
	CHK(r->sightings > 0);
	CHK(r->h.table.n > 0);
	CHK(r->cksum_bad == 0);   /* the padding restored evidence, it did not invent it */
}

int main(void)
{
	const char *root = getenv("REAC_CAPTURES");
	char base[512];
	if (root && *root) {
		snprintf(base, sizeof base, "%s", root);
	} else {
		const char *home = getenv("HOME");
		snprintf(base, sizeof base, "%s/Devel/audio/reac-captures", home ? home : "");
	}
	struct stat st;
	if (stat(base, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "skipping: no capture corpus at %s (set REAC_CAPTURES)\n", base);
		return 77;
	}

	char box_on_m[768], desk[768], filler_only[768];
	snprintf(box_on_m, sizeof box_on_m,
	         "%s/headamp-boxmaster-2026-09-11-slave/s1608-master-96k-vs-m200-slave-48k-30s.pcap", base);
	snprintf(desk, sizeof desk,
	         "%s/m200-master-441k-2026-09-11/box-boot-with-our-slave-present-12h03-12h06.pcap", base);
	snprintf(filler_only, sizeof filler_only,
	         "%s/s1608-misheard-as-master-2026-09-10.pcap", base);

	struct replay r;

	/* (c) A BOX IN M MASTERS — reac-captures ba9b5aa, "box on M slice". The S-1608's
	 * REAC Mode switch is on M: it broadcasts a master downstream at its OWN 16-channel
	 * width and announces itself with cfea, while the M-200 beside it is the slave.
	 * UNPINNED, we join it: a clock is a clock whichever end sends it, and the width it
	 * announces is what the segment is then sized from. */
	if (replay(box_on_m, -1, &r) != 0) {
		fprintf(stderr, "skipping: %s missing\n", box_on_m);
		return 77;
	}
	saw_a_wire(&r, "box-master-auto");
	CHK(r.padded > 0);                      /* this capture IS snaplen-512 */
	CHK(r.h.verdict == REAC_HUNT_SLAVE);
	CHK(reac_hunt_role(&r.h) == REAC_ROLE_SLAVE);
	CHK(r.h.arb.state == REAC_SEGMENT_FOREIGN);
	CHK(r.h.arb.rival == REAC_RIVAL_BOX);
	CHK(r.h.arb.rival_channels == REAC_BOX_S1608_IN);      /* the S-1608's own width, not a desk's 40 */
	CHK(r.h.arb.have_mac);
	CHK(memcmp(r.h.arb.mac, S1608_ON_M, 6) == 0);
	/* A box mastering is JOINED on an unpinned wire, so the daemon asserts the SLAVE end
	 * rather than a refusal — the refusal code below belongs to the PINNED arm and to
	 * nothing else. */
	CHK(reac_hunt_heard_anything(&r.h));

	/* THE SAME BYTES WITH THE SEGMENT PINNED MASTER — the shape of the 2026-09-16 live
	 * failure, and its fix. The daemon refused this wire that night: it published a door,
	 * served no audio, and the operator read "not detected". Since the operator's ruling of
	 * the same day ("we set the daemons to enroll any box, master or slave") a pin no
	 * longer changes the answer: the box is joined either way, at its own 16 channels.
	 * The switch position costs the head-amp, which the console reports beside a segment
	 * that works — it is not a reason to serve nothing. */
	CHK(replay(box_on_m, REAC_ROLE_MASTER, &r) == 0);
	saw_a_wire(&r, "box-master-pinned");
	CHK(r.h.verdict == REAC_HUNT_SLAVE);
	CHK(r.h.arb.rival == REAC_RIVAL_BOX);
	CHK(r.h.arb.rival_channels == REAC_BOX_S1608_IN);
	CHK(memcmp(r.h.arb.mac, S1608_ON_M, 6) == 0);
	/* THE ONE REFUSAL LEFT — a rival whose geometry has never been captured — is pinned by
	 * `test_reac_hunt.c`'s own unreadable-rival arm, on built frames, because the corpus
	 * holds no such rival to replay. That is what keeps the join above a decision about a
	 * BOX rather than the verdict going quiet. */

	/* (b) A DESK MASTERS — the M-200 holding the wire at the 40-channel downstream while
	 * our slave sits on the segment. The rival is a DESK by geometry alone; what `auto`
	 * then DOES with a desk is the courtship ruling's (option C: tap), and the detection
	 * below is what that ruling stands on. */
	if (replay(desk, -1, &r) != 0) {
		fprintf(stderr, "skipping: %s missing\n", desk);
		return 77;
	}
	saw_a_wire(&r, "desk-master");
	CHK(r.h.verdict == REAC_HUNT_SLAVE);
	CHK(r.h.arb.state == REAC_SEGMENT_FOREIGN);
	CHK(r.h.arb.rival == REAC_RIVAL_DESK);
	CHK(r.h.arb.rival_channels == REAC_MAX_CHANNELS);
	CHK(memcmp(r.h.arb.mac, M200, 6) == 0);

	/* THE ONE THAT MUST NOT BE A MASTER — "s1608 misheard as master", 2026-09-10: 3000
	 * frames of pure broadcast FILLER and not one control frame. A master's downstream
	 * audio is byte-identical in kind, so filler from a peer no control frame has proved
	 * is NOT evidence of a master. The name of the file is the bug this pins. */
	if (replay(filler_only, -1, &r) != 0) {
		fprintf(stderr, "skipping: %s missing\n", filler_only);
		return 77;
	}
	CHK(r.frames > 0);
	CHK(r.h.verdict != REAC_HUNT_SLAVE);
	CHK(r.h.arb.state != REAC_SEGMENT_FOREIGN);
	CHK(r.h.arb.rival != REAC_RIVAL_BOX);

	/* (d) SP — WHAT THE WIRE SHOWS, AND IT IS NOTHING (spec §3). Across every arm above
	 * the corpus carried zero SPLIT_ANNOUNCE, and the same pass counted cfea master
	 * announces in the thousands: the scan can see a control frame when one is there,
	 * which is what makes the zero a measurement instead of a broken filter. */
	unsigned long splits = 0, announces = 0;
	const char *all[3] = { box_on_m, desk, filler_only };
	for (int i = 0; i < 3; i++) {
		if (replay(all[i], -1, &r) != 0)
			continue;
		splits += r.splits;
		announces += r.announces;
	}
	CHK(announces > 0);      /* the positive control for the line below */
	CHK(splits == 0);

	/* And a SPLIT_ANNOUNCE the protocol describes but nobody has captured must not be
	 * able to flip a segment: libreac parses the kind and deliberately gives it NO role
	 * (arbitration §4). Built here rather than replayed, precisely because there is
	 * nothing to replay. */
	uint8_t synth[REAC_FRAME_BYTES];
	memset(synth, 0, sizeof synth);
	memcpy(synth, "\xff\xff\xff\xff\xff\xff", 6);
	memcpy(synth + 6, S1608_ON_M, 6);
	synth[REAC_ETHERTYPE_OFF] = REAC_ETHERTYPE >> 8; synth[REAC_ETHERTYPE_OFF + 1] = REAC_ETHERTYPE & 0xff;
	synth[16] = 0xce; synth[17] = 0xea;          /* the split device's own type word */
	reac_ctrl_checksum_apply(synth);
	struct reac_ctrl_parsed sp;
	CHK(reac_ctrl_parse(synth, sizeof synth, &sp) == REAC_CTRL_SPLIT_ANNOUNCE);
	struct reac_hunt h2;
	reac_hunt_init(&h2, OURS, 0);
	reac_hunt_observe(&h2, synth, sizeof synth, 1000, NULL);
	reac_hunt_step(&h2, 1000);
	CHK(h2.verdict == REAC_HUNT_HUNTING);
	CHK(h2.arb.state != REAC_SEGMENT_FOREIGN);
	CHK(h2.arb.rival != REAC_RIVAL_BOX && h2.arb.rival != REAC_RIVAL_DESK);

	if (fails == 0)
		printf("reac_hunt_captures: ok\n");
	return fails ? 1 : 0;
}
