// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test for reac_disco — the passive sighting classifier + table behind the
 * reac.discovery.* properties. Frames come from the reac_ctrl builders (the same
 * bytes the wire carries, checksum and all), never from a hand-rolled mock: the
 * false positive this whole module exists to prevent once SURVIVED a test suite
 * because the suite asserted it. Pins:
 *   (a) presence needs a real 0x8819 Roland-OUI frame — a busy link is not a device;
 *   (b) role comes from the full byte signature, not the frame KIND (cdea 04 03 is
 *       BOTH the box JOIN and the master grant; cdea 01 03 0010 is a BOX frame that
 *       parses as kind PROBE) — and stays UNKNOWN when the bytes are ambiguous;
 *   (c) a model is only ever the byte-exact config-block match, never inferred;
 *   (d) the table ages a vanished box out and bumps seq on real change only;
 *   (e) the JSON is a complete snapshot or nothing — never truncated. */
#include "reac_disco.h"
#include "reac_ctrl.h"
#include <reac/reac.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

	/* A 0x8819 frame from a NON-Roland OUI is not REAC gear. */
	n = reac_ctrl_build_box_hb(f, MASTER, BOX, 0x11, 16);
	CHK(n == 628);
	f[6] = 0xde; f[7] = 0xad; f[8] = 0xbe;      /* stomp the OUI */
	CHK(reac_disco_classify(f, n, OURS, &s) == -1);

	/* Our own echo is never a discovery of someone else. */
	n = reac_ctrl_build_box_hb(f, MASTER, OURS, 0x11, 16);
	CHK(reac_disco_classify(f, n, OURS, &s) == -1);

	/* A corrupt control block is evidence of NOTHING — not a device with a bad byte. */
	n = reac_ctrl_build_box_hb(f, MASTER, BOX, 0x11, 16);
	f[49] ^= 0xff;                               /* break the checksum */
	CHK(reac_ctrl_checksum_verify(f) != 0);
	CHK(reac_disco_classify(f, n, OURS, &s) == -1);

	/* ---- (b) role from the signature, not the kind. */

	/* The box keep-alive (cdea 01 03 0001 81) — unambiguously a box. */
	n = reac_ctrl_build_box_hb(f, MASTER, BOX, 0x11, 16);
	CHK(reac_disco_classify(f, n, OURS, &s) == 0);
	CHK(s.role == REAC_DISCO_ROLE_BOX);
	CHK(memcmp(s.mac, BOX, 6) == 0);
	/* Seen via a heartbeat, the model is NOT known. It must not be conjured from the
	 * 16-channel width (reac_box_model_by_channels would happily answer "s1608"). */
	CHK(s.model == NULL);

	/* The box config-announce (cdea 01 03 0010) parses as kind PROBE — a MASTER kind.
	 * Role must come from op_len, or every box that declares itself is filed as a rival
	 * master. */
	struct reac_ctrl_parsed p;
	n = reac_ctrl_build_config_announce(f, MASTER, BOX, 0x12, 8);   /* 8 in_ch = the S-0808 row */
	CHK(n > 0);
	CHK(reac_ctrl_parse(f, n, &p) == REAC_CTRL_PROBE);   /* the trap */
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
	n = reac_ctrl_build_flood_filler(f, BCAST, BOX, 0x13, 16, NULL, 12);   /* NULL = silent */
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

	/* ---- (d2) the RT announce gate: the event ring must not be flooded. */
	struct reac_disco_gate g;
	reac_disco_gate_init(&g);

	struct reac_disco_sighting live = { .role = REAC_DISCO_ROLE_BOX, .model = NULL };
	memcpy(live.mac, BOX, 6);

	CHK(reac_disco_gate_should_push(&g, &live, S_(1)) == 1);        /* first sight */

	/* An established box emits FILLER at 8000 fps. Simulate one second of it: exactly
	 * ONE refresh may pass, or the 128-slot ring evicts the JOIN/BYE transcript. */
	int pushes = 0;
	for (int i = 1; i <= 8000; i++) {
		uint64_t t_ns = S_(1) + (uint64_t)i * 125000ULL;   /* 8000 fps */
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

	printf("OK: disco — presence only from real 0x8819 Roland frames, role from the full "
	       "signature (never the kind), model never inferred, stale devices withdrawn, "
	       "JSON all-or-nothing\n");
	return 0;
}
