// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test for reac_disco — the passive sighting classifier + table behind the
 * reac.discovery.* properties. Frames come from the reac_ctrl builders (the same
 * bytes the wire carries, checksum and all), never from a hand-rolled mock: the
 * false positive this whole module exists to prevent once SURVIVED a test suite
 * because the suite asserted it. Pins:
 *   (a) presence needs a real 0x8819 frame — a busy link is not a device; NO MAC-vendor
 *       gate (deleted 2026-09-03, operator's ruling: discover by protocol frame only,
 *       never pin or spoof a MAC) — a non-Roland source is real evidence now;
 *   (a2) the FILLER gap that ruling opens is closed by SEGMENT-LEVEL PEER-LOCKING
 *       (reac_disco_classify_on_segment), not by any MAC-vendor scheme;
 *   (b) role comes from the full byte signature, not the frame KIND (cdea 04 03 is
 *       BOTH the box JOIN and the master grant; cdea 01 03 0010 is a BOX frame that
 *       parses as kind PROBE) — and stays UNKNOWN when the bytes are ambiguous;
 *   (c) a model is only ever the byte-exact config-block match, never inferred;
 *   (d) the table ages a vanished box out and bumps seq on real change only;
 *   (e) the JSON is a complete snapshot or nothing — never truncated. */
#include <reac/reac_disco.h>
#include <reac/reac_ctrl.h>
#include <reac/reac.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

#include "reac_s4000_golden.inc"

static const uint8_t MASTER[6] = { 0x00, 0x40, 0xab, 0x11, 0x22, 0x33 }; /* stand-in */
static const uint8_t BOX[6]    = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0xf6 }; /* stand-in */
static const uint8_t OURS[6]   = { 0x00, 0x40, 0xab, 0x99, 0x99, 0x99 }; /* this master */
static const uint8_t BCAST[6]  = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

#define S_(x) (1000000000ULL * (x))

int main(void)
{
	uint8_t f[1536];
	struct reac_disco_sighting s;
	size_t n;

	/* ---- (a) presence requires a real REAC frame. */

	/* An empty/838-byte lump of non-REAC traffic: not 0x8819, never a device. This is
	 * the #161 false positive in miniature — a busy link proves only that a link is
	 * busy. */
	memset(f, 0x5a, sizeof f);
	CHK(reac_disco_classify(f, 838, OURS, &s) == -1);

	/* A 0x8819 frame from a NON-Roland source IS real evidence now — there is no MAC-vendor
	 * gate. "Discover by protocol frame only, never pin or spoof a MAC" (operator's
	 * ruling, 2026-09-03): a valid checksummed control frame is real regardless of source
	 * OUI, and the sighting carries the real (non-Roland) source MAC verbatim. */
	n = reac_ctrl_build_box_hb(f, MASTER, BOX, 0x11, 16);
	CHK(n == REACPW_FRAME_LEN(16));
	uint8_t nonRoland[6] = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x01 };
	memcpy(f + 6, nonRoland, 6);
	reac_ctrl_checksum_apply(f);                 /* the source moved; keep it VALID */
	CHK(reac_disco_classify(f, n, OURS, &s) == 0);
	CHK(memcmp(s.mac, nonRoland, 6) == 0);

	/* Our own echo is never a discovery of someone else. */
	n = reac_ctrl_build_box_hb(f, MASTER, OURS, 0x11, 16);
	CHK(reac_disco_classify(f, n, OURS, &s) == -1);

	/* A corrupt control block is evidence of NOTHING — not a device with a bad byte. */
	n = reac_ctrl_build_box_hb(f, MASTER, BOX, 0x11, 16);
	f[49] ^= 0xff;                               /* break the checksum */
	CHK(reac_ctrl_checksum_verify(f) != 0);
	CHK(reac_disco_classify(f, n, OURS, &s) == -1);

	/* ---- (a2) SEGMENT-LEVEL PEER-LOCKING closes the FILLER gap the OUI removal opened.
	 * REAC is physically point-to-point, so once a segment's lock has latched to the source
	 * of its FIRST checksum-verified control frame, a FILLER (checksum-EXEMPT) frame from a
	 * DIFFERENT source on the SAME segment is refused — not an identity/vendor test, "is
	 * this the box already proven real on this wire". reac_disco_classify (no lock,
	 * everything above this point) is untouched by any of this. */
	struct reac_disco_peer_lock lock;
	reac_disco_peer_lock_init(&lock);
	CHK(lock.locked == 0);

	/* Before ANY control frame validates a peer, a FILLER is accepted exactly as the
	 * unlocked classifier already accepted it — the lock only closes the window AFTER a
	 * real peer is known. */
	n = reac_ctrl_build_upstream_filler(f, MASTER, BOX, 0x20, 16, NULL, REAC_SAMPLES_PER_PKT);
	CHK(n > 0);
	CHK(reac_disco_classify_on_segment(&lock, f, n, OURS, &s) == 0);
	CHK(s.role == REAC_DISCO_ROLE_BOX);
	CHK(lock.locked == 0);                      /* a FILLER never latches the lock itself */

	/* The box's real heartbeat (checksum-verified, non-FILLER) latches the lock to BOX. */
	n = reac_ctrl_build_box_hb(f, MASTER, BOX, 0x21, 16);
	CHK(reac_disco_classify_on_segment(&lock, f, n, OURS, &s) == 0);
	CHK(lock.locked == 1);
	CHK(memcmp(lock.mac, BOX, 6) == 0);

	/* Now a FILLER from the SAME box still passes. */
	n = reac_ctrl_build_upstream_filler(f, MASTER, BOX, 0x22, 16, NULL, REAC_SAMPLES_PER_PKT);
	CHK(reac_disco_classify_on_segment(&lock, f, n, OURS, &s) == 0);
	CHK(memcmp(s.mac, BOX, 6) == 0);

	/* A FILLER claiming to be a DIFFERENT box on the SAME (now-locked) segment is refused —
	 * this is the sabotage target: reverting classify_core's lock check must turn this red. */
	static const uint8_t IMPOSTOR[6] = { 0x00, 0x40, 0xab, 0xbe, 0xef, 0x01 };
	n = reac_ctrl_build_upstream_filler(f, MASTER, IMPOSTOR, 0x23, 16, NULL, REAC_SAMPLES_PER_PKT);
	CHK(reac_disco_classify_on_segment(&lock, f, n, OURS, &s) == -1);
	/* The SAME frame is real evidence through the unlocked classifier — the refusal is the
	 * lock's, not the frame's. */
	CHK(reac_disco_classify(f, n, OURS, &s) == 0);

	/* A LATER checksum-verified control frame from a different MAC is still accepted
	 * unchanged — the lock never restricts a VERIFIED frame, and it does not re-latch
	 * either (the first-proven peer holds for the lock's lifetime). */
	n = reac_ctrl_build_box_hb(f, MASTER, IMPOSTOR, 0x24, 16);
	CHK(reac_disco_classify_on_segment(&lock, f, n, OURS, &s) == 0);
	CHK(memcmp(s.mac, IMPOSTOR, 6) == 0);
	CHK(memcmp(lock.mac, BOX, 6) == 0);          /* still BOX — not re-latched */

	/* And the impostor's FILLER is STILL refused after that — proving a checksum-verified
	 * sighting from a new MAC does not quietly relax the lock. */
	n = reac_ctrl_build_upstream_filler(f, MASTER, IMPOSTOR, 0x25, 16, NULL, REAC_SAMPLES_PER_PKT);
	CHK(reac_disco_classify_on_segment(&lock, f, n, OURS, &s) == -1);

	/* A fresh lock (a segment drop/reopen) starts unlocked again, so the box's own FILLER
	 * is not permanently orphaned by a stale lock from a departed peer. */
	struct reac_disco_peer_lock lock2;
	reac_disco_peer_lock_init(&lock2);
	n = reac_ctrl_build_upstream_filler(f, MASTER, IMPOSTOR, 0x26, 16, NULL, REAC_SAMPLES_PER_PKT);
	CHK(reac_disco_classify_on_segment(&lock2, f, n, OURS, &s) == 0);

	/* ---- (b) role from the signature, not the kind. */

	/* The box keep-alive (cdea 01 03 0001 81) — unambiguously a box. */
	n = reac_ctrl_build_box_hb(f, MASTER, BOX, 0x11, 16);
	CHK(reac_disco_classify(f, n, OURS, &s) == 0);
	CHK(s.role == REAC_DISCO_ROLE_BOX);
	CHK(memcmp(s.mac, BOX, 6) == 0);
	/* Seen via a heartbeat, the model is NOT known. It must not be conjured from the
	 * 16-channel width (reac_box_model_by_channels would happily answer "s1608"). */
	CHK(s.model == NULL);

	/* The box config-announce (link 1, opcode 0x82/0x84) is its OWN kind now. It used
	 * to land in the parser's link-1 catch-all, a MASTER kind, and the role had to be
	 * dug back out of the length by hand — or every box that declares itself filed as
	 * a rival master. */
	struct reac_ctrl_parsed p;
	n = reac_ctrl_build_config_announce(f, MASTER, BOX, 0x12, 8);   /* 8 in_ch = the S-0808 row */
	CHK(n > 0);
	CHK(reac_ctrl_parse(f, n, &p) == REAC_CTRL_CONFIG_ANNOUNCE);
	CHK(p.link == REAC_LINK_CTRL && p.opcode == 0x84);   /* the 0x84 family */
	CHK(reac_disco_classify(f, n, OURS, &s) == 0);
	CHK(s.role == REAC_DISCO_ROLE_BOX);                  /* not master */

	/* ---- (c) the model is the byte-exact config-block match, and only that. */
	CHK(s.model != NULL);
	CHK(strcmp(s.model->token, "s0808") == 0);
	CHK(s.model->in_ch == 8 && s.model->out_ch == 8);

	/* A config-announce whose block matches no row stays unidentified rather than
	 * defaulting to S-1608. */
	n = reac_ctrl_build_config_announce(f, MASTER, BOX, 0x12, 8);
	f[30] ^= 0xff;                                       /* perturb inside the block */
	reac_ctrl_checksum_apply(f);                         /* keep it a VALID frame */
	CHK(reac_disco_classify(f, n, OURS, &s) == 0);
	CHK(s.role == REAC_DISCO_ROLE_BOX);
	CHK(s.model == NULL);                                /* unknown stays unknown */

	/* A broadcast FILLER is AMBIGUOUS: a box's presence-flood and a master's downstream
	 * audio are both type 0000 broadcast. Neither guess is honest. */
	n = reac_ctrl_build_flood_filler(f, BCAST, BOX, 0x13, 16, NULL, REAC_SAMPLES_PER_PKT);   /* NULL = silent */
	CHK(n > 0);
	CHK(reac_disco_classify(f, n, OURS, &s) == 0);
	CHK(s.role == REAC_DISCO_ROLE_UNKNOWN);
	CHK(memcmp(s.mac, BOX, 6) == 0);   /* the gear is real even when the role is not known */

	/* ---- (d) the table: change detection, ownership, aging. */
	struct reac_disco_table t;
	reac_disco_table_init(&t);
	CHK(t.n == 0 && t.seq == 0);

	struct reac_disco_sighting box = { .role = REAC_DISCO_ROLE_BOX, .model = NULL };
	memcpy(box.mac, BOX, 6);

	CHK(reac_disco_table_observe(&t, &box, 1, S_(1)) == 1);   /* new MAC = a change */
	CHK(t.n == 1 && t.seq == 1);
	CHK(t.e[0].owned == 1 && t.e[0].first_seen_ns == S_(1));

	/* A re-sighting is not a change — it must not churn seq (openmixer would read every
	 * bump as new discovery data). */
	CHK(reac_disco_table_observe(&t, &box, 1, S_(2)) == 0);
	CHK(t.seq == 1 && t.n == 1);
	CHK(t.e[0].last_seen_ns == S_(2));         /* but liveness DID advance */
	CHK(t.e[0].first_seen_ns == S_(1));        /* first_seen never moves */

	/* Learning the model later IS a change. */
	box.model = reac_box_model_by_token("s0808");
	CHK(box.model != NULL);
	CHK(reac_disco_table_observe(&t, &box, 1, S_(3)) == 1);
	CHK(t.seq == 2);
	CHK(t.e[0].model == box.model);

	/* A second, unowned device coexists. */
	struct reac_disco_sighting rival = { .role = REAC_DISCO_ROLE_MASTER, .model = NULL };
	memcpy(rival.mac, MASTER, 6);
	CHK(reac_disco_table_observe(&t, &rival, 0, S_(3)) == 1);
	CHK(t.n == 2 && t.seq == 3);

	/* Nothing is stale yet at +4 s. */
	CHK(reac_disco_table_age(&t, S_(4)) == 0);
	CHK(t.n == 2);

	/* The box keeps talking; the rival goes quiet and is withdrawn at >5 s of silence.
	 * A vanished device must DISAPPEAR — a stagebox standing stale is the "no alarm"
	 * failure. */
	CHK(reac_disco_table_observe(&t, &box, 1, S_(8)) == 0);
	CHK(reac_disco_table_age(&t, S_(9)) == 1);
	CHK(t.n == 1 && t.seq == 4);               /* a withdrawal is a change */
	CHK(memcmp(t.e[0].mac, BOX, 6) == 0);      /* the survivor is the one still talking */

	/* Overflow saturates and SAYS so — a full table is never a complete picture. */
	reac_disco_table_init(&t);
	struct reac_disco_sighting many = { .role = REAC_DISCO_ROLE_BOX, .model = NULL };
	memcpy(many.mac, BOX, 6);
	for (int i = 0; i < REAC_DISCO_MAX + 3; i++) {
		many.mac[5] = (uint8_t)i;
		reac_disco_table_observe(&t, &many, 0, S_(1));
	}
	CHK(t.n == REAC_DISCO_MAX);
	CHK(t.overflowed == 1);

	/* ---- (d3) WIDEST WINS, AND IT MUST NOT DEPEND ON THE MODEL. A peer's control frames
	 * carry no audio geometry, so its DATA frames are the only answer about how wide it is
	 * — and a box strapped to master mode declares no model at all (it emits no config
	 * announce), so a width that only widened when the MODEL changed never widened for
	 * exactly the peer whose width decides the segment's topology. Found 2026-09-09 reading
	 * this merge for the box-master join: the widening branch sat inside the model branch. */
	reac_disco_table_init(&t);
	struct reac_disco_sighting narrow = { .role = REAC_DISCO_ROLE_UNKNOWN, .model = NULL,
	                                      .channels = 8 };
	memcpy(narrow.mac, MASTER, 6);
	CHK(reac_disco_table_observe(&t, &narrow, 0, S_(1)) == 1);   /* a new MAC */
	CHK(t.e[0].channels == 8);
	struct reac_disco_sighting wide = narrow;
	wide.channels = 32;
	CHK(reac_disco_table_observe(&t, &wide, 0, S_(2)) == 1);     /* wider IS a change */
	CHK(t.e[0].channels == 32);
	CHK(t.seq == 2);
	/* ...and a later narrow frame does not take it back: a control frame with no geometry
	 * would otherwise erase what the data frames established. */
	CHK(reac_disco_table_observe(&t, &narrow, 0, S_(3)) == 0);
	CHK(t.e[0].channels == 32);

	/* ---- (d2) the RT announce gate: the event ring must not be flooded. */
	struct reac_disco_gate g;
	reac_disco_gate_init(&g);

	struct reac_disco_sighting live = { .role = REAC_DISCO_ROLE_BOX, .model = NULL };
	memcpy(live.mac, BOX, 6);

	CHK(reac_disco_gate_should_push(&g, &live, S_(1)) == 1);        /* first sight */

	/* An established box emits FILLER at 8000 fps. Simulate one second of it: exactly
	 * ONE refresh may pass, or the 128-slot ring evicts the JOIN/BYE transcript. */
	int pushes = 0;
	for (int i = 1; i <= REAC_PKT_RATE_96K; i++) {
		uint64_t t_ns = S_(1) + (uint64_t)i * (1000000000ULL / REAC_PKT_RATE_96K);   /* one 96 kHz slot */
		if (reac_disco_gate_should_push(&g, &live, t_ns))
			pushes++;
	}
	CHK(pushes == 1);            /* 8000 frames -> 1 event */

	/* A sharpened fact is an EDGE and jumps the window immediately — the operator sees
	 * the box identify itself now, not up to a second later. */
	live.model = reac_box_model_by_token("s0808");
	CHK(reac_disco_gate_should_push(&g, &live, S_(1) + 1000000ULL) == 1);
	/* …but the same fact repeated is not an edge. */
	CHK(reac_disco_gate_should_push(&g, &live, S_(1) + 2000000ULL) == 0);

	/* An ambiguous frame after a definite one never un-learns the role. */
	struct reac_disco_sighting vague = { .role = REAC_DISCO_ROLE_UNKNOWN, .model = NULL };
	memcpy(vague.mac, BOX, 6);
	CHK(reac_disco_gate_should_push(&g, &vague, S_(1) + 3000000ULL) == 0);
	CHK(g.e[0].role == REAC_DISCO_ROLE_BOX);

	/* A model index survives the round-trip through the ring's byte-sized slot. */
	CHK(reac_disco_model_index(NULL) == -1);
	CHK(reac_disco_model_by_index(-1) == NULL);
	const struct reac_box_model *m0808 = reac_box_model_by_token("s0808");
	CHK(reac_disco_model_by_index(reac_disco_model_index(m0808)) == m0808);

	/* ---- (e) JSON: a complete snapshot, or nothing at all. */
	reac_disco_table_init(&t);
	box.model = reac_box_model_by_token("s0808");
	CHK(reac_disco_table_observe(&t, &box, 1, S_(10)) == 1);

	char buf[1024];
	int len = reac_disco_table_json(&t, S_(10) + 120000000ULL /* +120 ms */, buf, sizeof buf);
	CHK(len > 0 && (size_t)len < sizeof buf);
	CHK(buf[0] == '[' && buf[len - 1] == ']');
	CHK(strstr(buf, "\"mac\":\"00:40:ab:c4:80:f6\"") != NULL);
	CHK(strstr(buf, "\"role\":\"box\"") != NULL);
	CHK(strstr(buf, "\"model\":\"s0808\"") != NULL);
	CHK(strstr(buf, "\"width\":\"8x8\"") != NULL);
	CHK(strstr(buf, "\"owned\":true") != NULL);
	CHK(strstr(buf, "\"age_ms\":120") != NULL);

	/* An unidentified device serializes honestly — "unknown"/"0x0", not a made-up model. */
	reac_disco_table_init(&t);
	box.model = NULL;
	CHK(reac_disco_table_observe(&t, &box, 0, S_(10)) == 1);
	len = reac_disco_table_json(&t, S_(10), buf, sizeof buf);
	CHK(len > 0);
	CHK(strstr(buf, "\"model\":\"unknown\"") != NULL);
	CHK(strstr(buf, "\"width\":\"0x0\"") != NULL);
	CHK(strstr(buf, "\"owned\":false") != NULL);

	/* An empty table is a well-formed empty array: "reac-pw looked and saw nothing" is a
	 * real, publishable answer — distinct from publishing no property at all. */
	reac_disco_table_init(&t);
	len = reac_disco_table_json(&t, S_(1), buf, sizeof buf);
	CHK(len == 2 && strcmp(buf, "[]") == 0);

	/* Too small a buffer yields -1, never a half-written list that parses as a shorter
	 * (and wrong) device set. */
	reac_disco_table_init(&t);
	box.model = reac_box_model_by_token("s0808");
	reac_disco_table_observe(&t, &box, 1, S_(10));
	CHK(reac_disco_table_json(&t, S_(10), buf, 20) == -1);

	/* ---- (f) GOLDEN REPLAY: real S-4000 frames, byte-verbatim (issue #90).
	 *
	 * Every entry is a captured frame with the verdict the classifier must
	 * reach — the box cold-connect escalation is BOX in all four steps
	 * (0016/001a used to file as a rival master), an uncaptured cdea 01
	 * declaration stays UNKNOWN, and a head-amp record is master evidence.
	 * Provenance and the known grant-echo ambiguity: reac_s4000_golden.inc. */
	for (size_t i = 0; i < S4000_GOLD_COUNT; i++) {
		const struct s4000_gold *e = &S4000_GOLD[i];
		memset(f, 0, sizeof f);
		memcpy(f, e->head, sizeof e->head);
		if (reac_disco_classify(f, e->len, OURS, &s) != 0) {
			fprintf(stderr, "FAIL: golden %zu did not classify\n", i);
			return 1;
		}
		if ((int)s.role != e->role) {
			fprintf(stderr, "FAIL: golden %zu role %s, expected %s\n", i,
			        reac_disco_role_name(s.role),
			        reac_disco_role_name((enum reac_disco_role)e->role));
			return 1;
		}
		if (e->model == NULL ? s.model != NULL
		                     : (s.model == NULL || strcmp(s.model->token, e->model) != 0)) {
			fprintf(stderr, "FAIL: golden %zu model %s, expected %s\n", i,
			        s.model ? s.model->token : "(none)",
			        e->model ? e->model : "(none)");
			return 1;
		}
	}

	/* ---- (g) SPLIT_ANNOUNCE (ce ea): a splitter's announce is REAL GEAR with
	 * an UNPROVEN role. The frame is source-derived (reac-aes67
	 * REAC-PROTOCOL.md §10.1: first-announce data[0..8], the split's MAC at
	 * data[9..14], block checksummed like every announce) — no capture of one
	 * exists yet (§14.1, the last unmapped type), so the role must stay
	 * UNKNOWN: a frame kind nobody has captured must not flip the segment's
	 * topology. The MAC is still a sighting, like the ambiguous flood above. */
	static const uint8_t SPLIT[6] = { 0x00, 0x40, 0xab, 0x77, 0x77, 0x77 }; /* stand-in */
	static const uint8_t SPLIT_FIRST[9] =
		{ 0x01, 0x00, 0x7f, 0x00, 0x01, 0x03, 0x08, 0x43, 0x05 };
	memset(f, 0, sizeof f);
	memcpy(f, MASTER, 6);
	memcpy(f + 6, SPLIT, 6);
	f[REAC_ETHERTYPE_OFF] = REAC_ETHERTYPE >> 8; f[REAC_ETHERTYPE_OFF + 1] = REAC_ETHERTYPE & 0xff;
	f[16] = 0xce; f[17] = 0xea;
	memcpy(f + 18, SPLIT_FIRST, sizeof SPLIT_FIRST);
	memcpy(f + 27, SPLIT, 6);                 /* data[9..14] = the split's MAC */
	reac_ctrl_checksum_apply(f);
	CHK(reac_ctrl_parse(f, 64, &p) == REAC_CTRL_SPLIT_ANNOUNCE);   /* named, not UNKNOWN_CTRL */
	CHK(reac_disco_classify(f, 64, OURS, &s) == 0);
	CHK(s.role == REAC_DISCO_ROLE_UNKNOWN);
	CHK(memcmp(s.mac, SPLIT, 6) == 0);
	CHK(s.model == NULL);

	/* ---- (e) THE GEOMETRY RIDES THE SIGHTING (arbitration §2b).
	 *
	 * A classifier reads what a peer SAYS; the frame length says what it IS. Recording the
	 * width here is what lets arbitration tell a desk (40 ch) from a stagebox strapped to
	 * master mode (its own, smaller width) — the two want opposite responses, and no control
	 * frame distinguishes them. 0 means the frame carried no legal geometry, which is a fact,
	 * not a zero width. */
	n = reac_ctrl_build_box_hb(f, MASTER, BOX, 0x11, 16);
	CHK(reac_disco_classify(f, n, OURS, &s) == 0);
	CHK(s.channels == reac_frame_channels((size_t)n));

	printf("OK: disco — presence from a real 0x8819 frame with NO MAC-vendor gate, a locked "
	       "segment refusing an impostor's FILLER, role from the full signature (never the "
	       "kind), model never inferred, stale devices withdrawn, JSON all-or-nothing, "
	       "S-4000 goldens replay byte-verbatim\n");
	return 0;
}
