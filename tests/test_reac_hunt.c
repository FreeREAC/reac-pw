// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_hunt — which end of the pairing a HEARD segment takes when nobody configured
 * one — the `auto` role, which is the default (reac_hunt.h).
 *
 * THE DEFECT THIS EXISTS AGAINST, 2026-09-08: reac-pw was launched with `REAC_ROLE=slave`
 * standing in a conf file as the floor for every segment, and both of the rig's boxes —
 * an S-1608 and an S-4000S, powered, cabled and waiting to be granted — sat there while
 * the daemon hunted for a master that was never going to speak. Only an explicit master
 * role brought them up. A wire with a box on it and no master IS a wire we drive, and no
 * file should have to say so.
 *
 * Frames are built with the libreac builders — the bytes the wire carries, checksum and
 * all — never hand-rolled, because the classifier's own false positives are what this
 * decision now rests on. The four outcomes are driven by real geometry.
 */
#include <reac/reac_hunt.h>

#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>

#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

#define SEC 1000000000ULL

static const uint8_t OURS[6]  = { 0x34, 0x5a, 0x60, 0x9f, 0x9e, 0xbe };  /* this rig's NIC */
static const uint8_t DESK[6]  = { 0x00, 0x40, 0xab, 0xc9, 0xcc, 0x03 };  /* a real M-200 */
static const uint8_t BOX[6]   = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x41 };  /* the S-1608 */
static const uint8_t BOXM[6]  = { 0x00, 0x40, 0xab, 0xc4, 0x08, 0xbc };  /* the S-4000S on M */
static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* A box's own heartbeat: unambiguous BOX evidence, 16-channel geometry. */
static int box_heartbeat(struct reac_hunt *h, uint64_t now)
{
	uint8_t f[2048];
	size_t n = reac_ctrl_build_box_hb(f, DESK, BOX, 0x11, 16);
	return reac_hunt_observe(h, f, n, now, NULL);
}

/* A box that has lost its master: BROADCAST filler at wire rate, which classifies as
 * role UNKNOWN (a master's downstream audio is byte-identical in kind) and is separated
 * from one only by its WIDTH. */
static int box_flood(struct reac_hunt *h, const uint8_t src[6], int n_ch, uint64_t now)
{
	uint8_t f[2048];
	size_t n = reac_ctrl_build_flood_filler(f, BCAST, src, 0x20, n_ch, NULL, 12);
	return reac_hunt_observe(h, f, n, now, NULL);
}

/* A BOX'S OWN BYE: link 1, SINGLE, opcode 0x00 (REAC_OP_BULK) — the exact four header
 * bytes a master's SCENE_TRANSFER bulk push uses (reac_ctrl.c's documented residue: the
 * box's disconnect and the master's scene push are told apart only by DIRECTION, and
 * role_of() — the discovery-time classifier, a promiscuous tap by construction — has no
 * direction check to make that call with). Sent when a box gives up on a lost master. */
static int box_bye(struct reac_hunt *h, const uint8_t src[6], uint64_t now)
{
	uint8_t f[50];
	memset(f, 0, sizeof f);
	memcpy(f, BCAST, 6);
	memcpy(f + 6, src, 6);
	f[12] = 0x88; f[13] = 0x19;
	f[16] = 0xcd; f[17] = 0xea;
	uint8_t *block = f + 18;
	block[0] = 0x01;   /* REAC_LINK_CTRL */
	block[1] = 0x03;   /* REAC_SEG_SINGLE */
	block[4] = 0x00;   /* REAC_OP_BULK */
	reac_ctrl_checksum_apply(f);
	return reac_hunt_observe(h, f, sizeof f, now, NULL);
}

/* A DESK: only a console emits head-amp records, and it emits them at the 40-channel
 * downstream width. Role master AND desk geometry, in one frame. */
static int desk_headamp(struct reac_hunt *h, const uint8_t src[6], uint64_t now)
{
	uint8_t f[2048];
	size_t n = reac_ctrl_build_headamp(f, BCAST, src, 0x30, 0x20, 0 /* phantom */, 1);
	if (n == 0)
		return -2;
	return reac_hunt_observe(h, f, n, now, NULL);
}

/* A STAGEBOX STRAPPED TO MASTER: the same master-only record, emitted at the box's OWN
 * width — measured 2026-08-30 as 1204 B on a wire where a desk had emitted 1492 B. The
 * frame claims master; the geometry says box; the geometry wins. */
static int box_on_m(struct reac_hunt *h, uint64_t now)
{
	uint8_t f[2048];
	size_t n = reac_ctrl_build_flood_filler(f, BCAST, BOXM, 0x40, 32, NULL, 12);
	if (n == 0 || reac_ctrl_stamp_headamp(f, 0x20, 0 /* phantom */, 1) != 0)
		return -2;
	reac_ctrl_checksum_apply(f);
	return reac_hunt_observe(h, f, n, now, NULL);
}

/* A RIVAL WITH NO READABLE GEOMETRY: a master-only record on a frame whose length matches
 * no legal `52 + n*36` chassis. Nobody has captured such a peer, which is exactly §4's
 * case — it is neither driven over nor joined. */
static int rival_no_geometry(struct reac_hunt *h, uint64_t now)
{
	uint8_t f[2048];
	size_t n = reac_ctrl_build_headamp(f, BCAST, BOXM, 0x30, 0x20, 0 /* phantom */, 1);
	if (n == 0)
		return -2;
	return reac_hunt_observe(h, f, 64, now, NULL);
}

int main(void)
{
	struct reac_hunt h;
	uint64_t t0 = 100 * SEC;

	/* ---- A. A SILENT WIRE IS NEVER TAKEN. Link is the gate to listen; HEARING is the
	 * gate to serve. A NIC that carries no REAC frame is not a
	 * segment however long we wait, and driving needs evidence. */
	reac_hunt_init(&h, OURS, t0);
	CHK(h.verdict == REAC_HUNT_HUNTING);
	CHK(reac_hunt_step(&h, t0 + 10 * SEC) == 0);
	CHK(h.verdict == REAC_HUNT_HUNTING);
	CHK(reac_hunt_heard_anything(&h) == 0);

	/* ---- B. A BOX AND NO MASTER: WE DRIVE — but only after the window.
	 * No foreign master: we drive, probe, grant, establish. */
	reac_hunt_init(&h, OURS, t0);
	CHK(box_heartbeat(&h, t0) == 1);          /* a new peer: an observable change */
	CHK(reac_hunt_heard_anything(&h) == 1);
	/* Inside the window nothing is taken. A desk announces once a second; taking the
	 * wire before three of its cadences have passed is a race we would sometimes win
	 * against a desk that was there all along. */
	CHK(reac_hunt_step(&h, t0 + 1) == 0);
	CHK(h.verdict == REAC_HUNT_HUNTING);
	CHK(reac_hunt_step(&h, t0 + REAC_HUNT_WINDOW_NS - 1) == 0);
	CHK(h.verdict == REAC_HUNT_HUNTING);
	/* The window closes on a wire that never answered: it is ours. */
	CHK(box_heartbeat(&h, t0 + REAC_HUNT_WINDOW_NS) >= 0);   /* still live */
	CHK(reac_hunt_step(&h, t0 + REAC_HUNT_WINDOW_NS) == 1);  /* CHANGED: worth a line */
	CHK(h.verdict == REAC_HUNT_MASTER);
	CHK(reac_hunt_role(&h) == REAC_ROLE_MASTER);
	CHK(strcmp(reac_hunt_verdict_name(h.verdict), "master") == 0);
	/* Said once: the second step agrees and reports no change. */
	CHK(reac_hunt_step(&h, t0 + REAC_HUNT_WINDOW_NS + SEC / 2) == 0);

	/* ---- B2. THE WINDOW IS ANCHORED ON THE FIRST SIGHTING, not on the socket. A sniffer
	 * that has watched a quiet NIC for an hour must still spend three cadences HEARING a
	 * segment that has just powered up — otherwise the first box to speak takes a wire
	 * whose desk is two seconds behind it. */
	reac_hunt_init(&h, OURS, t0);
	CHK(reac_hunt_step(&h, t0 + 3600 * SEC) == 0);          /* an hour of silence */
	CHK(box_heartbeat(&h, t0 + 3600 * SEC) == 1);           /* now a box appears */
	CHK(reac_hunt_step(&h, t0 + 3600 * SEC + SEC) == 0);    /* one cadence: not yet */
	CHK(h.verdict == REAC_HUNT_HUNTING);
	CHK(box_heartbeat(&h, t0 + 3600 * SEC + REAC_HUNT_WINDOW_NS) == 0);
	CHK(reac_hunt_step(&h, t0 + 3600 * SEC + REAC_HUNT_WINDOW_NS) == 1);
	CHK(h.verdict == REAC_HUNT_MASTER);

	/* ---- C. THE OUTAGE'S OWN SHAPE. A box whose master went away floods BROADCAST
	 * filler, which classifies UNKNOWN — so a rule that waited for an unambiguous BOX
	 * role would hunt forever with a box in plain sight. Its 16-channel width is not
	 * ambiguous at all. */
	reac_hunt_init(&h, OURS, t0);
	CHK(box_flood(&h, BOX, 16, t0) == 1);
	CHK(reac_hunt_step(&h, t0 + REAC_HUNT_WINDOW_NS) == 1);
	CHK(h.verdict == REAC_HUNT_MASTER);

	/* ---- D. A DESK MASTERS IT: WE JOIN AS SLAVE, and we do not wait out the window to
	 * do it — a desk on the wire is not a maybe, and a desk is JOINED, never refused. */
	reac_hunt_init(&h, OURS, t0);
	CHK(desk_headamp(&h, DESK, t0) == 1);
	CHK(reac_hunt_step(&h, t0 + SEC / 10) == 1);
	CHK(h.verdict == REAC_HUNT_SLAVE);
	CHK(reac_hunt_role(&h) == REAC_ROLE_SLAVE);
	CHK(h.arb.state == REAC_SEGMENT_FOREIGN);
	CHK(h.arb.rival == REAC_RIVAL_DESK);
	CHK(memcmp(h.arb.mac, DESK, 6) == 0);

	/* ---- E. A STAGEBOX MASTERS AN UNPINNED WIRE: WE JOIN IT (operator, 2026-09-09).
	 * This file used to assert the opposite, and the opposite is what the rig proved
	 * wrong: an S-0808 on M was refused, nothing was served, and the segment vanished
	 * from the console. A clock is a clock whichever end of the pairing is sending it,
	 * so the wire is obeyed — and the width it announces is carried out of the verdict,
	 * because the segment's nodes are sized from it. */
	reac_hunt_init(&h, OURS, t0);
	CHK(box_on_m(&h, t0) == 1);
	CHK(reac_hunt_step(&h, t0 + SEC / 10) == 1);
	CHK(h.verdict == REAC_HUNT_SLAVE);
	CHK(reac_hunt_role(&h) == REAC_ROLE_SLAVE);
	CHK(h.arb.state == REAC_SEGMENT_FOREIGN);
	CHK(h.arb.rival == REAC_RIVAL_BOX);
	CHK(h.arb.rival_channels == 32);          /* the S-4000S on M: 1204 B frames */
	CHK(strcmp(reac_rival_refusal(h.arb.rival), "rival-master-box") == 0);  /* the CODE stands */
	CHK(memcmp(h.arb.mac, BOXM, 6) == 0);
	/* And it stays joined past the window: a wire with a master on it is not vacant. */
	CHK(reac_hunt_step(&h, t0 + REAC_HUNT_WINDOW_NS + SEC) == 0);
	CHK(h.verdict == REAC_HUNT_SLAVE);

	/* ---- E2. AN UNREADABLE RIVAL IS STILL REFUSED. §4's conservatism: a peer that
	 * claims master while carrying no legal `52 + n*36` geometry has not been captured
	 * by anybody, and a frame kind nobody has captured must not flip a segment's
	 * topology — not into driving it, and not into joining it either. */
	reac_hunt_init(&h, OURS, t0);
	CHK(rival_no_geometry(&h, t0) == 1);
	CHK(reac_hunt_step(&h, t0 + SEC / 10) == 1);
	CHK(h.verdict == REAC_HUNT_REFUSED);
	CHK(h.arb.rival == REAC_RIVAL_UNKNOWN);
	CHK(h.arb.rival_channels == 0);
	CHK(strcmp(reac_rival_refusal(h.arb.rival), "rival-master-unknown") == 0);

	/* ---- F. A 40-CHANNEL STREAM WHOSE OWNER HAS NOT ANNOUNCED IS NOT A VACANT WIRE.
	 * A desk's downstream audio classifies UNKNOWN exactly as a box's flood does; only
	 * the width separates them, and 40 is the master downstream and nothing else.
	 * Taking that wire is the two-masters fault, so the window does NOT expire into it. */
	reac_hunt_init(&h, OURS, t0);
	CHK(box_flood(&h, DESK, REAC_MAX_CHANNELS, t0) == 1);
	CHK(reac_hunt_step(&h, t0 + REAC_HUNT_WINDOW_NS + SEC) == 0);
	CHK(h.verdict == REAC_HUNT_HUNTING);
	/* Its announce arrives one cadence later and settles it: slave. */
	CHK(desk_headamp(&h, DESK, t0 + REAC_HUNT_WINDOW_NS + SEC) >= 0);
	CHK(reac_hunt_step(&h, t0 + REAC_HUNT_WINDOW_NS + SEC) == 1);
	CHK(h.verdict == REAC_HUNT_SLAVE);

	/* ---- G. NOTHING LATCHES. The desk is unplugged and the box is still there: the
	 * sightings age out on the table's own staleness bar and the same wire becomes ours.
	 * A verdict that could not be revisited would need a restart to notice a cable. */
	uint64_t t1 = t0 + REAC_HUNT_WINDOW_NS + SEC;
	CHK(box_heartbeat(&h, t1) == 1);                       /* the box arrives beside it */
	uint64_t t2 = t1 + REAC_DISCO_STALE_NS + SEC;          /* the desk stops talking */
	CHK(box_heartbeat(&h, t2) == 0);                       /* the box does not: liveness only */
	CHK(reac_hunt_step(&h, t2) == 1);
	CHK(h.verdict == REAC_HUNT_MASTER);

	/* ---- H. OUR OWN ECHO IS NOT EVIDENCE OF ANYBODY. A hub or a loopback that hands
	 * our own master traffic back must not make us slave to ourselves. */
	reac_hunt_init(&h, OURS, t0);
	CHK(desk_headamp(&h, OURS, t0) == -1);      /* not a sighting at all */
	CHK(reac_hunt_step(&h, t0 + REAC_HUNT_WINDOW_NS) == 0);
	CHK(h.verdict == REAC_HUNT_HUNTING);
	CHK(reac_hunt_heard_anything(&h) == 0);

	/* ---- I. A PIN IS SERVED WITHOUT A HUNT AND WITHOUT A FRAME. `REAC_ROLE_<segment>`
	 * is an answer about this wire — a SETTING, not a guess — so it waits on nothing at
	 * all: the pinned role is the verdict from the first step after link. Making a
	 * pinned segment sit out the window, or find box evidence, would be the daemon
	 * second-guessing a setting; and a pinned segment the hunt could not decide would
	 * never be served at all, which is the shape of the outage this all comes from.
	 *
	 * THE COLD-BOX DEFECT, measured 2026-09-08 22:10 on 0.5.0-2 (DESIGN.md, "A cold
	 * stagebox is silent"): the S-0808 and the S-1608 were powered, cabled and carrier
	 * up, and neither emitted a single 0x8819 frame — a REAC box in slave mode says
	 * nothing until a master announces to IT. So a pin that waited for "the wire to BE a
	 * segment" waited forever, and both boxes stayed mute. THE FIRST STEP DECIDES, on an
	 * utterly silent table, or a pinned master can never wake the box it was pinned for.
	 * PROVEN ON THE RIG with 0.5.0-3: both boxes came up within two seconds of
	 * "pinned master — driving on link". */
	reac_hunt_init(&h, OURS, t0);
	reac_hunt_pin(&h, REAC_ROLE_MASTER);
	CHK(reac_hunt_step(&h, t0) == 1);              /* silent wire, zero frames, no window */
	CHK(h.verdict == REAC_HUNT_MASTER);
	CHK(reac_hunt_role(&h) == REAC_ROLE_MASTER);
	CHK(reac_hunt_heard_anything(&h) == 0);        /* and it is honest about hearing nothing */
	/* A pinned SLAVE opens its own side on the same silence: the operator's
	 * `REAC_ROLE_<iface>=slave` is obeyed on link too, with no frame waited for.
	 *
	 * AND IT DOES TRANSMIT. This comment used to say a pinned slave "transmits nothing
	 * until a master is heard" and that is FALSE: on PHY-up the slave role FLOODS
	 * REAC_FSM_FLOOD_BURST (~5460) broadcast FILLER frames — reac_fsm.h's
	 * FLOOD_ANNOUNCE, reac_slave.c — and only falls quiet afterwards if nothing answered.
	 * That flood is the protocol's own bounded cold-connect announcement and is exactly
	 * what a real box does, so it is correct; it is simply not silence, and the journal
	 * line says "cold-connect flood, then listening for a master". */
	reac_hunt_init(&h, OURS, t0);
	reac_hunt_pin(&h, REAC_ROLE_SLAVE);
	CHK(reac_hunt_step(&h, t0) == 1);
	CHK(h.verdict == REAC_HUNT_SLAVE);
	CHK(reac_hunt_role(&h) == REAC_ROLE_SLAVE);
	/* AND THE UNPINNED PATH IS UNTOUCHED: a silent wire nobody answered for is still
	 * passive after the whole window. This is the control that keeps the fix from being
	 * "drive every NIC in the house". */
	reac_hunt_init(&h, OURS, t0);
	CHK(reac_hunt_step(&h, t0 + 10 * SEC) == 0);
	CHK(h.verdict == REAC_HUNT_HUNTING);
	reac_hunt_init(&h, OURS, t0);
	reac_hunt_pin(&h, REAC_ROLE_MASTER);
	CHK(box_flood(&h, BOX, 16, t0) == 1);
	CHK(reac_hunt_step(&h, t0 + SEC / 100) == 1);  /* 10 ms in, not 3 s */
	CHK(h.verdict == REAC_HUNT_MASTER);
	CHK(reac_hunt_role(&h) == REAC_ROLE_MASTER);

	/* A pinned SLAVE likewise, and on a wire with no desk on it at all: the operator
	 * said be a box here, and the daemon does not require evidence of a master before
	 * obeying. Whether the wire agrees is the listener's own arbitration to publish
	 * (the intent-versus-observation disagreement, which needs the segment up to exist). */
	reac_hunt_init(&h, OURS, t0);
	reac_hunt_pin(&h, REAC_ROLE_SLAVE);
	CHK(box_flood(&h, BOX, 16, t0) == 1);
	CHK(reac_hunt_step(&h, t0 + SEC / 100) == 1);
	CHK(h.verdict == REAC_HUNT_SLAVE);
	CHK(reac_hunt_role(&h) == REAC_ROLE_SLAVE);

	/* A PINNED MASTER BESIDE A BOX ON M IS THE ONE CONTRADICTION, and it is REFUSED
	 * (operator, 2026-09-09). Everywhere else the wire is obeyed; here the operator has
	 * written down that this segment is ours to drive, and a box says it is not. The
	 * daemon does not settle that by out-shouting a box — it refuses, names the code, and
	 * the remedy is the box's own switch.
	 *
	 * IT STILL WAITS FOR NOTHING. The refusal fires on the FIRST step, from whatever the
	 * table already holds — a box on M streams at wire cadence, so it is there within
	 * microseconds of the sniffer opening — and a wire whose box is COLD leaves the table
	 * empty and the pin drives, which is the cold-start rule, untouched. */
	reac_hunt_init(&h, OURS, t0);
	reac_hunt_pin(&h, REAC_ROLE_MASTER);
	CHK(box_on_m(&h, t0) == 1);
	CHK(reac_hunt_step(&h, t0 + SEC / 100) == 1);
	CHK(h.verdict == REAC_HUNT_REFUSED);
	CHK(h.arb.rival == REAC_RIVAL_BOX);
	CHK(h.arb.rival_channels == 32);
	CHK(memcmp(h.arb.mac, BOXM, 6) == 0);
	/* Nothing latches here either: the box is switched to S and stops mastering, the
	 * sighting ages out, and the pin drives the wire it was pinned for. */
	CHK(reac_hunt_step(&h, t0 + REAC_DISCO_STALE_NS + SEC) == 1);
	CHK(h.verdict == REAC_HUNT_MASTER);

	/* A PINNED SLAVE JOINS THE SAME BOX: the pin and the wire agree, and there is
	 * nothing to refuse — we were never going to drive this segment. */
	reac_hunt_init(&h, OURS, t0);
	reac_hunt_pin(&h, REAC_ROLE_SLAVE);
	CHK(box_on_m(&h, t0) == 1);
	CHK(reac_hunt_step(&h, t0 + SEC / 100) == 1);
	CHK(h.verdict == REAC_HUNT_SLAVE);

	/* AND §4 CUTS BOTH WAYS FOR A PIN: an UNREADABLE rival does not flip a pinned
	 * master into a refusal. Only a BOX is read sharply enough to out-rank a pin — its
	 * geometry is unambiguous and its remedy is a switch on its front. */
	reac_hunt_init(&h, OURS, t0);
	reac_hunt_pin(&h, REAC_ROLE_MASTER);
	CHK(rival_no_geometry(&h, t0) == 1);
	CHK(reac_hunt_step(&h, t0 + SEC / 100) == 1);
	CHK(h.verdict == REAC_HUNT_MASTER);

	/* ---- J. THE MASTERLESS LICENCE, for a wire nobody pinned. reac_knock proves a wire
	 * carried NOTHING for its observation window; a master fills every audio slot and
	 * cannot be present and silent, so that is proof there is no master. The hunt then
	 * drives it WITHOUT hearing a box first — which is the requirement that left two
	 * powered, cabled rig boxes mute, because a cold box spends a bounded flood on PHY-up
	 * and can never afterwards be the evidence its own waking depends on. */
	reac_hunt_init(&h, OURS, t0);
	CHK(reac_hunt_step(&h, t0 + REAC_HUNT_WINDOW_NS) == 0);   /* no licence: not taken */
	CHK(h.verdict == REAC_HUNT_HUNTING);
	reac_hunt_silence_proven(&h);
	CHK(reac_hunt_step(&h, t0 + REAC_HUNT_WINDOW_NS) == 1);
	CHK(h.verdict == REAC_HUNT_MASTER);
	CHK(reac_hunt_role(&h) == REAC_ROLE_MASTER);
	CHK(reac_hunt_heard_anything(&h) == 0);   /* honest: it drove having heard nothing */

	/* AND EVIDENCE STILL OUTRANKS THE LICENCE, which is the whole safety half. A desk
	 * that turns up on a wire we were licensed to drive is JOINED, never out-shouted; so
	 * is a stagebox strapped to master, on the same terms and for the same reason. The licence only
	 * ever replaces "wait three cadences and find a box". */
	reac_hunt_init(&h, OURS, t0);
	reac_hunt_silence_proven(&h);
	CHK(desk_headamp(&h, DESK, t0) == 1);
	CHK(reac_hunt_step(&h, t0 + SEC / 100) == 1);
	CHK(h.verdict == REAC_HUNT_SLAVE);
	reac_hunt_init(&h, OURS, t0);
	reac_hunt_silence_proven(&h);
	CHK(box_on_m(&h, t0) == 1);
	CHK(reac_hunt_step(&h, t0 + SEC / 100) == 1);
	CHK(h.verdict == REAC_HUNT_SLAVE);      /* a box that masters it is JOINED (0.5.1) */
	reac_hunt_init(&h, OURS, t0);
	reac_hunt_silence_proven(&h);
	CHK(rival_no_geometry(&h, t0) == 1);
	CHK(reac_hunt_step(&h, t0 + SEC / 100) == 1);
	CHK(h.verdict == REAC_HUNT_REFUSED);    /* an unreadable one still is not */

	/* ---- K. A BOX DOES NOT UN-PROVE ITSELF (misheard-as-master, S-1608
	 * 00:40:ab:c4:80:41, 2026-09-10 10:22:23 — reac-captures/
	 * s1608-misheard-as-master-2026-09-10.pcap). The box is heard unambiguously
	 * (a heartbeat); it then gives up on its lost master and sends its own BYE, whose
	 * four header bytes classify by KIND as a master's SCENE_TRANSFER. Before the fix
	 * this flipped the table entry to MASTER and the hunt joined the box AS ITS SLAVE
	 * — exactly the log line reac-pw printed on the rig: "box masters this wire —
	 * joining it as a slave". The remedy is not occupancy (both frames are the same
	 * 16-channel geometry throughout): a box already proven by unambiguous evidence
	 * does not reclassify to MASTER on one later ambiguous-shaped frame from the same
	 * MAC. */
	reac_hunt_init(&h, OURS, t0);
	CHK(box_heartbeat(&h, t0) == 1);
	CHK(reac_hunt_step(&h, t0) == 0);
	CHK(h.verdict == REAC_HUNT_HUNTING);
	CHK(box_bye(&h, BOX, t0 + SEC) == 0);     /* NOT an observable change: refused */
	CHK(reac_hunt_step(&h, t0 + SEC) == 0);
	CHK(h.verdict != REAC_HUNT_SLAVE);
	CHK(h.arb.state != REAC_SEGMENT_FOREIGN);
	/* And the box is still exactly what it was: a masterless wire with a box on it is
	 * ours to drive once the window closes, never a wire to join as a slave. */
	CHK(reac_hunt_step(&h, t0 + REAC_HUNT_WINDOW_NS) == 1);
	CHK(h.verdict == REAC_HUNT_MASTER);

	/* K2. POSITIVE CONTROL — an UNAMBIGUOUS master (a real desk's head-amp record,
	 * never emitted by a box) still classifies MASTER and is still joined, whether or
	 * not this MAC was ever heard as a box before. The fix narrows one specific
	 * ambiguous-shape collision; it does not blunt real master evidence. */
	reac_hunt_init(&h, OURS, t0);
	CHK(desk_headamp(&h, DESK, t0) == 1);
	CHK(reac_hunt_step(&h, t0 + SEC / 10) == 1);
	CHK(h.verdict == REAC_HUNT_SLAVE);
	CHK(h.arb.rival == REAC_RIVAL_DESK);

	/* ---- The window itself, stated as the number and its reason: three master announce
	 * cadences, and a cadence is one second (reac_master.c: announce_tick >= fps). */
	CHK(REAC_HUNT_WINDOW_NS == 3 * SEC);
	CHK(REAC_HUNT_WINDOW_NS < REAC_DISCO_STALE_NS);

	printf("ok: a vacant wire is taken after %llu s, a desk is joined, a box on M is "
	       "joined too unless the wire is pinned master (which refuses it), an unreadable "
	       "rival is refused either way, a pin drives on link with no frame at all, a wire "
	       "proven silent is driven while evidence still outranks that, nothing latches\n",
	       (unsigned long long)(REAC_HUNT_WINDOW_NS / SEC));
	return 0;
}
