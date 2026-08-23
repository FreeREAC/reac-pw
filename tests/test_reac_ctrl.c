// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_ctrl builders/parser/checksum, validated against the real wire WITHOUT
 * embedding any rig MAC: the box-heartbeat control block is MAC-independent, so
 * its checksum byte (0x7a, observed on wire in reac-captures) is a fixed
 * cross-check of the checksum algorithm. Geometry (628 B box width, 00 7a
 * descriptor, C2 EA trailer) is ground-truthed in REAC-CONNECTION-FSM.md. */
#include "reac_ctrl.h"
#include <reac/reac_upstream.h>
#include <reac/reac.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static const uint8_t MASTER[6] = { 0x00, 0x40, 0xab, 0x11, 0x22, 0x33 }; /* stand-in */
static const uint8_t SRC[6]    = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0xf6 }; /* our stand-in */

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* THE TWO CHECKSUMS, IN THE RIGHT ORDER.
 *
 * An op-0403 record carries a Roland DT1 checksum (INNER, sum-to-0x80) at a byte
 * that the REAC control-block checksum (OUTER, sum-to-0) also covers, so the
 * inner one MUST be stamped first. A record finished outer-first — or finished
 * with the block helper alone — looks perfect on the wire and the box rejects
 * it. This has cost real rig time, so it is pinned here rather than left to a
 * comment in the builder.
 *
 * Asserting only that both checksums verify could pass by luck, so every case
 * also BUILDS the frame an outer-first implementation would emit (clear the
 * inner byte, stamp the outer checksum, then stamp the inner one) and requires
 * that frame to be detectably broken. Reverse the builder's order and the
 * positive half fails; drop the inner stamp and the record half fails. */
static int check_record_cksum_order(void)
{
	static const struct { uint8_t ch, param, value; } RECS[] = {
		{ 0x00, REAC_HEADAMP_PHANTOM, 0x00 },
		{ 0x00, REAC_HEADAMP_PHANTOM, 0x01 },
		{ 0x07, REAC_HEADAMP_PAD,     0x00 },
		{ 0x07, REAC_HEADAMP_PAD,     0x01 },
		{ 0x20, REAC_HEADAMP_SENS,    0x00 },   /* S-1608 base: input 1  */
		{ 0x2f, REAC_HEADAMP_SENS,    0x1a },   /* S-1608 base: input 16 */
		{ 0x10, REAC_HEADAMP_SENS,    REAC_HEADAMP_SENS_MAX },
	};
	static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	uint8_t f[1536], wrong[1536];
	int traps = 0;

	for (size_t i = 0; i < sizeof(RECS) / sizeof(RECS[0]); i++) {
		/* (a) as the builder emits it: BOTH checksums hold at once, which is
		 * only satisfiable by stamping the inner one before the outer one. */
		size_t n = reac_ctrl_build_headamp(f, BCAST, SRC, 0x77,
		                                   RECS[i].ch, RECS[i].param, RECS[i].value);
		CHK(n == REAC_FRAME_BYTES);
		CHK(f[36] == RECS[i].ch && f[37] == RECS[i].param && f[38] == RECS[i].value);
		CHK(reac_ctrl_headamp_record_verify(f) == 0);   /* INNER: sum-to-0x80 */
		CHK(reac_ctrl_checksum_verify(f) == 0);         /* OUTER: sum-to-0    */

		/* (b) the same record finished OUTER-FIRST: the inner stamp lands on a
		 * byte the outer sum has already counted, so the block no longer sums
		 * to 0 — the frame the box would reject. */
		memcpy(wrong, f, n);
		wrong[39] = 0x00;                            /* un-stamp the inner byte  */
		reac_ctrl_checksum_apply(wrong);             /* OUTER first (the bug)    */
		reac_ctrl_record_cksum_stamp(wrong + 34, 6); /* INNER after it           */
		CHK(reac_ctrl_headamp_record_verify(wrong) == 0);    /* record looks fine */
		if (wrong[39] != 0x00) {          /* a zero inner byte clobbers nothing */
			CHK(reac_ctrl_checksum_verify(wrong) != 0);  /* ...the block does not */
			CHK(memcmp(wrong, f, n) != 0);
			traps++;
		}

		/* (c) the in-place stamp path finishes in the same order — it overlays
		 * the record on a built FILLER and must leave both checksums valid. */
		size_t m = reac_ctrl_build_upstream_filler(f, MASTER, SRC, 0x88, 16, NULL, 12);
		CHK(m == 628);
		CHK(reac_ctrl_stamp_headamp(f, RECS[i].ch, RECS[i].param, RECS[i].value) == 0);
		CHK(reac_ctrl_headamp_record_verify(f) == 0);
		CHK(reac_ctrl_checksum_verify(f) == 0);
		CHK(f[626] == 0xc2 && f[627] == 0xea);   /* the tail survives the stamp */
	}
	/* the negative half must actually have fired, or (b) proves nothing */
	CHK(traps > 0);
	return 0;
}

int main(void)
{
	uint8_t f[1536];

	/* 1. box heartbeat: cdea 01 03 0001 81, 628 B, checksum == 0x7a (wire value) */
	size_t n = reac_ctrl_build_box_hb(f, MASTER, SRC, 0x1234, 16);
	CHK(n == 628);
	CHK(f[16] == 0xcd && f[17] == 0xea);
	CHK(f[18] == 0x01 && f[19] == 0x03 && f[20] == 0x00 && f[21] == 0x01 && f[22] == 0x81);
	CHK(f[626] == 0xc2 && f[627] == 0xea);
	CHK(reac_ctrl_checksum_verify(f) == 0);
	CHK(f[49] == 0x7a);   /* <-- cross-check vs the real captured box heartbeat */

	struct reac_ctrl_parsed p;
	CHK(reac_ctrl_parse(f, n, &p) == REAC_CTRL_BOX_HB);
	CHK(p.counter == 0x1234);
	CHK(p.link == REAC_LINK_CTRL && p.seg == REAC_SEG_SINGLE &&
	    p.opcode == REAC_OP_BOX_HB && p.blk_len == 1);
	CHK(!p.is_broadcast && memcmp(p.src, SRC, 6) == 0 && memcmp(p.dst, MASTER, 6) == 0);

	/* 1b. the heartbeat width follows box_channels (W3): 8-ch = 340 B, 40-ch = 1492 B,
	 * odd / out-of-range rejected. Byte-length = 50 hdr + n_ch*36 + 2 end. */
	CHK(reac_ctrl_build_box_hb(f, MASTER, SRC, 7, 8) == 340);
	CHK(f[338] == 0xc2 && f[339] == 0xea && reac_ctrl_checksum_verify(f) == 0);
	CHK(reac_ctrl_build_box_hb(f, MASTER, SRC, 7, 40) == 1492);
	CHK(reac_ctrl_build_box_hb(f, MASTER, SRC, 7, 15) == 0);   /* odd widths don't exist */
	CHK(reac_ctrl_build_box_hb(f, MASTER, SRC, 7, 0)  == 0);
	CHK(reac_ctrl_build_box_hb(f, MASTER, SRC, 7, 42) == 0);   /* > 40 */

	/* 2. upstream FILLER: 628 B, type 0000, 00 7a descriptor, audio round-trips
	 * through the capture-verified upstream decoder — i.e. we emit the same
	 * BRAIDED layout a real box does (task #108), not plain LE. */
	float chbuf[16][12]; float *pl[16];
	for (int c = 0; c < 16; c++) {
		pl[c] = chbuf[c];
		for (int s = 0; s < 12; s++)
			chbuf[c][s] = (float)c / 64.0f - 0.1f + (float)s / 1024.0f;
	}
	n = reac_ctrl_build_upstream_filler(f, MASTER, SRC, 0x2222, 16, pl, 12);
	CHK(n == 628);
	CHK(f[16] == 0x00 && f[17] == 0x00);
	for (int k = 0; k < 16; k++) CHK(f[18 + 2 * k] == 0x00 && f[18 + 2 * k + 1] == 0x7a);
	CHK(f[626] == 0xc2 && f[627] == 0xea);
	uint8_t pcm[16 * 12 * 3];
	CHK(reac_upstream_decode(f, n, pcm) == 12);
	float maxerr = 0;
	for (int ch = 0; ch < 16; ch++)
		for (int s = 0; s < 12; s++) {
			const uint8_t *p3 = &pcm[(size_t)(ch * 12 + s) * 3];
			int32_t v = p3[0] | (p3[1] << 8) | (p3[2] << 16);
			if (v & 0x800000) v |= ~0xffffff;
			float got = (float)v / 8388608.0f, e = fabsf(got - chbuf[ch][s]);
			if (e > maxerr) maxerr = e;
		}
	CHK(maxerr < 1e-6f);
	/* odd widths don't exist on-wire (the braid packs pairs) -> rejected */
	CHK(reac_ctrl_build_upstream_filler(f, MASTER, SRC, 0x2222, 15, pl, 12) == 0);

	/* 3. experimental JOIN builders: checksum invariant holds */
	reac_ctrl_build_config_announce(f, MASTER, SRC, 1, 16);
	CHK(reac_ctrl_checksum_verify(f) == 0);
	reac_ctrl_build_coldconnect(f, MASTER, SRC, 1, 16, NULL, 12);
	CHK(reac_ctrl_checksum_verify(f) == 0 && f[18] == 0x04 && f[19] == 0x03);

	/* 3b. the full cold-connect escalation 0014->0013->0016->001a, byte-matched to
	 * a real S-1608 (2026-07-11). block[31] = frame[49] is the per-variant trailer. */
	n = reac_ctrl_build_coldconnect_0013(f, MASTER, SRC, 1, 16, NULL, 12);
	CHK(n == 628 && f[20] == 0x00 && f[21] == 0x13 && f[49] == 0x02);  /* was 0x00 (bug) */
	n = reac_ctrl_build_coldconnect_0016(f, MASTER, SRC, 1, 16, NULL, 12);
	CHK(n == 628 && f[20] == 0x00 && f[21] == 0x16 && f[49] == 0xfc);
	n = reac_ctrl_build_coldconnect_001a(f, MASTER, SRC, 1, 16, NULL, 12);
	CHK(n == 628 && f[20] == 0x00 && f[21] == 0x1a && f[49] == 0xf4);
	/* 8-ch width -> 340 B; odd widths rejected */
	CHK(reac_ctrl_build_coldconnect_0016(f, MASTER, SRC, 1, 8, NULL, 12) == 340);
	CHK(reac_ctrl_build_coldconnect_001a(f, MASTER, SRC, 1, 15, NULL, 12) == 0);

	/* 4. 8-channel box width -> 340 B */
	n = reac_ctrl_build_upstream_filler(f, MASTER, SRC, 1, 8, NULL, 12);
	CHK(n == 340);

	/* 5. master-side box-frame classifier truth table -----------------------
	 * OUR_MAC is the master's identity; SRC plays the box. The canonical zoneA
	 * JOIN is what reac_ctrl_build_coldconnect now emits. */
	static const uint8_t OUR_MAC[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };
	static const uint8_t BCAST[6]   = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	enum reac_master_rx_event ev;

	/* (a) the canonical cold-connect matches as JOIN, unicast AND broadcast */
	n = reac_ctrl_build_coldconnect(f, OUR_MAC, SRC, 7, 16, NULL, 12);
	CHK(f[18] == 0x04 && f[19] == 0x03 && f[20] == 0x00 && f[21] == 0x14 &&
	    f[22] == 0x00 && f[23] == 0x02);                     /* zoneA block head */
	CHK(reac_ctrl_classify_box_frame(f, n, OUR_MAC, &p, &ev) == 0);
	CHK(ev == REAC_M_RX_BOX_JOIN);
	CHK(p.link == REAC_LINK_RECORD && p.seg == REAC_SEG_SINGLE &&
	    p.dt1_tag == REAC_DT1_TAG_JOIN);                     /* the tag, not the length */
	memcpy(f, BCAST, 6);                                     /* broadcast variant */
	CHK(reac_ctrl_classify_box_frame(f, n, OUR_MAC, &p, &ev) == 0);
	CHK(ev == REAC_M_RX_BOX_JOIN && p.is_broadcast);

	/* (b) THE LENGTH DECIDES NOTHING. Rewriting block[2:4] to any value at all
	 * leaves the record a JOIN, because what makes it one is its DT1 tag. The old
	 * matcher read this field and accepted a hand-kept set of four values, which
	 * is what a container that is not full breaks. */
	static const uint8_t LENS[] = { 0x13, 0x15, 0x1f, 0x00 };
	for (size_t li = 0; li < sizeof LENS / sizeof LENS[0]; li++) {
		n = reac_ctrl_build_coldconnect(f, OUR_MAC, SRC, 7, 16, NULL, 12);
		f[21] = LENS[li];
		reac_ctrl_checksum_apply(f);
		CHK(reac_ctrl_classify_box_frame(f, n, OUR_MAC, &p, &ev) == 0);
		CHK(ev == REAC_M_RX_BOX_JOIN && p.blk_len == LENS[li]);
	}

	/* (c) rejects: corrupted checksum / an unknown DT1 tag / our own echo /
	 * non-Roland. The tag is the field that can refuse now — a link-4 record whose
	 * register page we have never captured must not close a grant window. */
	n = reac_ctrl_build_coldconnect(f, OUR_MAC, SRC, 7, 16, NULL, 12);
	f[49] ^= 0x5a;                                           /* break the checksum */
	CHK(reac_ctrl_classify_box_frame(f, n, OUR_MAC, &p, &ev) == -1);
	n = reac_ctrl_build_coldconnect(f, OUR_MAC, SRC, 7, 16, NULL, 12);
	f[34] = 0x07; f[35] = 0x77;                              /* an uncaptured tag */
	reac_ctrl_checksum_apply(f);
	CHK(reac_ctrl_parse(f, n, &p) == REAC_CTRL_GRANT && p.dt1_tag == 0x0777);
	CHK(reac_ctrl_classify_box_frame(f, n, OUR_MAC, &p, &ev) == -1);
	n = reac_ctrl_build_coldconnect(f, OUR_MAC, OUR_MAC, 7, 16, NULL, 12); /* src == our_mac */
	CHK(reac_ctrl_classify_box_frame(f, n, OUR_MAC, &p, &ev) == -1);
	n = reac_ctrl_build_coldconnect(f, OUR_MAC, SRC, 7, 16, NULL, 12);
	f[6] = 0xde; f[7] = 0xad;                                /* non-Roland OUI */
	CHK(reac_ctrl_classify_box_frame(f, n, OUR_MAC, &p, &ev) == -1);

	/* (d) NOT keyed on the tail: mutated inventory byte still matches (the 0x41
	 * is device inventory, never a MAC tail) */
	n = reac_ctrl_build_coldconnect(f, OUR_MAC, SRC, 7, 16, NULL, 12);
	f[28] = 0x99;                                            /* block[10]: 0x41->0x99 */
	reac_ctrl_checksum_apply(f);
	CHK(reac_ctrl_classify_box_frame(f, n, OUR_MAC, &p, &ev) == 0);
	CHK(ev == REAC_M_RX_BOX_JOIN);

	/* (e) box hb opcode 0x81 -> HEARTBEAT (the box's "I am locked" signal); opcode
	 *     0x00 -> BYE; bcast FILLER -> presence; unicast upstream FILLER -> UNICAST.
	 *     The BYE's opcode is the bulk-transfer opcode, so libreac classifies the
	 *     frame as SCENE_TRANSFER and only DIRECTION tells the two apart — see the
	 *     note in reac_ctrl_classify_box_frame. */
	n = reac_ctrl_build_box_hb(f, OUR_MAC, SRC, 9, 16);
	CHK(reac_ctrl_classify_box_frame(f, n, OUR_MAC, &p, &ev) == 0);
	CHK(ev == REAC_M_RX_BOX_HEARTBEAT);
	n = reac_ctrl_build_box_hb(f, OUR_MAC, SRC, 9, 16);
	f[22] = 0x00;                                            /* disconnect latch */
	reac_ctrl_checksum_apply(f);
	CHK(reac_ctrl_parse(f, n, &p) == REAC_CTRL_SCENE_TRANSFER);   /* the collision */
	CHK(reac_ctrl_classify_box_frame(f, n, OUR_MAC, &p, &ev) == 0);
	CHK(ev == REAC_M_RX_BOX_BYE);
	n = reac_ctrl_build_upstream_filler(f, BCAST, SRC, 9, 16, NULL, 12);
	CHK(reac_ctrl_classify_box_frame(f, n, OUR_MAC, &p, &ev) == 0);
	CHK(ev == REAC_M_RX_BOX_BCAST_FILLER);
	n = reac_ctrl_build_upstream_filler(f, OUR_MAC, SRC, 9, 16, NULL, 12);
	CHK(reac_ctrl_classify_box_frame(f, n, OUR_MAC, &p, &ev) == 0);
	CHK(ev == REAC_M_RX_BOX_UNICAST);
	/* a config-announce (cdea 01 03 0010) is the box's SETUP DECLARATION — its
	 * own event so the master FSM can establish on it (warm relink). */
	n = reac_ctrl_build_config_announce(f, OUR_MAC, SRC, 9, 16);
	CHK(reac_ctrl_classify_box_frame(f, n, OUR_MAC, &p, &ev) == 0);
	CHK(ev == REAC_M_RX_BOX_CONFIG);
	/* a unicast between OTHER parties is not ours */
	n = reac_ctrl_build_box_hb(f, MASTER, SRC, 9, 16);
	CHK(reac_ctrl_classify_box_frame(f, n, OUR_MAC, &p, &ev) == -1);

	/* 7. MASTER-side box recognition: a box's own config-announce round-trips
	 * back to its matrix model (slave emits -> master identifies the same row). */
	struct { const char *tok; int in_ch; } cases[] = {
		{ "s1608", 16 }, { "s0808", 8 }, { "s4000s", 32 },
	};
	for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
		n = reac_ctrl_build_config_announce(f, MASTER, SRC, 0x55, cases[i].in_ch);
		const struct reac_box_model *m = reac_ctrl_identify_box(f, n);
		CHK(m != NULL);
		CHK(strcmp(m->token, cases[i].tok) == 0);
		CHK(m->in_ch == cases[i].in_ch);
	}
	/* a NON-config-announce frame (heartbeat) is not identifiable -> NULL */
	n = reac_ctrl_build_box_hb(f, MASTER, SRC, 0x55, 16);
	CHK(reac_ctrl_identify_box(f, n) == NULL);
	/* an unknown 0x84 descriptor (mutate one descriptor byte) -> NULL (falls back) */
	n = reac_ctrl_build_config_announce(f, MASTER, SRC, 0x55, 8);
	f[REAC_CTRL_BLOCK_OFF + 8] ^= 0xff;   /* corrupt a descriptor byte */
	CHK(reac_ctrl_identify_box(f, n) == NULL);

	/* 8. the record/block checksum ORDER the builder scaffold makes structural */
	if (check_record_cksum_order())
		return 1;


	/* 9. THE RETIRED --box PIN'S ONE REMAINING JOB: say once that what somebody
	 * typed disagrees with what the box declared. Exactly once is the contract —
	 * the box repeats its config-announce, so a per-frame notice becomes thousands
	 * of identical lines and stops being read, while never saying it is how a wrong
	 * pin sat in reac.env unnoticed for a session. */
	{
		const char *pin = "s1608";
		CHK(reac_box_pin_notice(&pin, "s0808") == 1);   /* disagrees -> notice */
		CHK(pin == NULL);                                /* consumed */
		CHK(reac_box_pin_notice(&pin, "s0808") == 0);   /* and never again */

		pin = "s1608:Drums";                             /* the :label is not the model */
		CHK(reac_box_pin_notice(&pin, "s0808") == 1);
		pin = "s1608:Drums";
		CHK(reac_box_pin_notice(&pin, "s1608") == 0);   /* agrees -> silence */
		CHK(pin == NULL);                                /* still consumed */

		/* a prefix must not read as agreement in either direction */
		pin = "s16";
		CHK(reac_box_pin_notice(&pin, "s1608") == 1);
		pin = "s1608";
		CHK(reac_box_pin_notice(&pin, "s16") == 1);

		/* nothing typed, or nothing recognized yet: nothing to say */
		pin = NULL;
		CHK(reac_box_pin_notice(&pin, "s0808") == 0);
		pin = "s1608";
		CHK(reac_box_pin_notice(&pin, NULL) == 0);
		CHK(pin != NULL);        /* NOT consumed: we have not recognized anything */
		CHK(reac_box_pin_notice(NULL, "s0808") == 0);
	}

	/* SPLIT_ANNOUNCE (type ce ea) is a NAMED kind, not UNKNOWN_CTRL — the
	 * split role's own announce (reac-aes67 REAC-PROTOCOL.md §6/§10.1,
	 * source-derived from reacdriver; no capture exists yet, §14.1). All
	 * three documented payload forms carry the same type word; the parser
	 * keys on the type word alone. Every announce is block-checksummed. */
	{
		static const uint8_t FORMS[3][9] = {
			{ 0x01, 0x00, 0x7f, 0x00, 0x01, 0x03, 0x08, 0x43, 0x05 },  /* first     */
			{ 0x01, 0x00, 0x02, 0x00, 0x01, 0x03, 0x08, 0x42, 0x05 },  /* second    */
			{ 0x01, 0x00, 0x02, 0x00, 0x01, 0x03, 0x02, 0x41, 0x05 },  /* keep-alive */
		};
		for (int k = 0; k < 3; k++) {
			memset(f, 0, 64);
			memcpy(f, MASTER, 6);
			memcpy(f + 6, SRC, 6);
			f[12] = 0x88; f[13] = 0x19;
			f[16] = 0xce; f[17] = 0xea;
			memcpy(f + 18, FORMS[k], sizeof FORMS[k]);
			memcpy(f + 27, SRC, 6);          /* data[9..14] = the split's MAC */
			reac_ctrl_checksum_apply(f);
			CHK(reac_ctrl_parse(f, 64, &p) == REAC_CTRL_SPLIT_ANNOUNCE);
			CHK(reac_ctrl_checksum_verify(f) == 0);
		}
	}

	/* ---- the scene push: a body must survive the transfer whole -------------
	 * The box completes reassembly only when the payload bytes it has stitched
	 * together reach the total the header declared, and it runs its state-4 COMMIT
	 * there and nowhere else. So the one property that matters is that walking
	 * every step and concatenating the payloads gives back exactly the body, with
	 * the declared total agreeing. This is the truncation class killed in a unit
	 * test — no rig, no box, no timing. */
	{
		uint8_t body[REAC_SCENE_BYTES];
		for (size_t i = 0; i < sizeof body; i++)
			body[i] = (uint8_t)(i * 7 + (i >> 5));   /* not the placeholder */

		uint8_t back[REAC_SCENE_BYTES];
		size_t got = 0;
		int chunks = 0, total = -1;

		for (int step = 0; step < REAC_SCENE_STEPS; step++) {
			uint8_t blk[34];
			CHK(reac_ctrl_build_scene_step(blk, body, sizeof body, step) == 0);
			CHK(blk[0] == 0xcd && blk[1] == 0xea);
			/* the block checksum rule holds for every step */
			unsigned sum = 0;
			for (int i = 2; i < 34; i++)
				sum += blk[i];
			CHK((sum & 0xff) == 0);

			int len = (blk[4] << 8) | blk[5];
			if (step == 0) {
				CHK(blk[2] == 0x01 && blk[3] == 0x01);
				CHK(len == REAC_SCENE_HEAD_BYTES);
				total = (blk[7] << 8) | blk[8];
				memcpy(back + got, blk + 9, REAC_SCENE_HEAD_BYTES);
				got += REAC_SCENE_HEAD_BYTES;
			} else if (step < REAC_SCENE_STEPS - 1) {
				CHK(blk[2] == 0x01 && blk[3] == 0x00);
				CHK(len == REAC_SCENE_CHUNK_BYTES);
				memcpy(back + got, blk + 7, REAC_SCENE_CHUNK_BYTES);
				got += REAC_SCENE_CHUNK_BYTES;
				chunks++;
			} else {
				CHK(blk[2] == 0x01 && blk[3] == 0x02);
				CHK(len == REAC_SCENE_TAIL_BYTES);
				memcpy(back + got, blk + 7, REAC_SCENE_TAIL_BYTES);
				got += REAC_SCENE_TAIL_BYTES;
			}
		}

		CHK(chunks == REAC_SCENE_CHUNKS);          /* 341, not 174 */
		CHK(got == REAC_SCENE_BYTES);              /* 8904 recovered */
		CHK(total == REAC_SCENE_BYTES);            /* and that is what we declared */
		CHK(memcmp(back, body, sizeof body) == 0); /* byte for byte */

		/* The framing constants are ONE fact, not four: the payload lengths must
		 * sum to the total the header declares, or a box that trusts the header
		 * waits forever for bytes that are never coming. */
		CHK(REAC_SCENE_HEAD_BYTES +
		    REAC_SCENE_CHUNKS * REAC_SCENE_CHUNK_BYTES +
		    REAC_SCENE_TAIL_BYTES == REAC_SCENE_BYTES);

		/* A body that is not a whole transfer is refused, never half-sent. */
		uint8_t blk[34];
		CHK(reac_ctrl_build_scene_step(blk, body, sizeof body - 1, 0) == -1);
		CHK(reac_ctrl_build_scene_step(blk, body, sizeof body, -1) == -1);
		CHK(reac_ctrl_build_scene_step(blk, body, sizeof body, REAC_SCENE_STEPS) == -1);

		/* THE THREE TAGS THE COMMIT VALIDATES. The box compares these four-byte
		 * spans before promoting anything and silently promotes NOTHING if one
		 * misses. Two of them ride middle chunks, which is exactly where the old
		 * synthetic pattern went, so this assertion is the regression that keeps a
		 * future generated body from re-breaking the promotion invisibly. */
		uint8_t gen[REAC_SCENE_BYTES];
		CHK(reac_ctrl_scene_build(gen, sizeof gen, SRC) == 0);
		CHK(memcmp(gen + REAC_SCENE_TAG_ID_OFF,   "1234", 4) == 0);
		CHK(memcmp(gen + REAC_SCENE_TAG_SYSP_OFF, "SYSP", 4) == 0);
		CHK(memcmp(gen + REAC_SCENE_TAG_SCEN_OFF, "SCEN", 4) == 0);
		CHK(memcmp(gen + REAC_SCENE_MAC_OFF, SRC, 6) == 0);
		CHK(reac_ctrl_scene_build(gen, sizeof gen - 1, SRC) == -1);
		/* and they must survive the chunker onto the wire, not just exist in the
		 * body: SYSP rides chunk 32 and SCEN chunk 33. */
		{
			uint8_t c32[34], c33[34];
			CHK(reac_ctrl_build_scene_step(c32, gen, REAC_SCENE_BYTES, 33) == 0);
			CHK(reac_ctrl_build_scene_step(c33, gen, REAC_SCENE_BYTES, 34) == 0);
			CHK(memmem(c32 + 7, 26, "SYSP", 4) != NULL);
			CHK(memmem(c33 + 7, 26, "SCEN", 4) != NULL);
		}

		/* Our identity goes into the body, replacing the capturing desk's. */
		uint8_t mine[REAC_SCENE_BYTES];
		CHK(reac_ctrl_scene_build(mine, sizeof mine, SRC) == 0);
		CHK(reac_ctrl_scene_set_mac(mine, sizeof mine, SRC) == 0);
		CHK(memcmp(mine + REAC_SCENE_MAC_OFF, SRC, 6) == 0);
		CHK(reac_ctrl_scene_set_mac(mine, sizeof mine - 1, SRC) == -1);
	}

	printf("OK: reac_ctrl builders byte-faithful (box-hb checksum 0x7a matches wire), "
	       "parser + descriptor + audio round-trip + box-frame classifier clean, "
	       "DT1 record checksum stamped before the block checksum, "
	       "retired --box pin disagreement reported exactly once, "
	       "scene push round-trips 8904 B in 341 chunks\n");
	return 0;
}
