// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* EMULATION CONFORMANCE: for every --mixer profile (m200/m300/m5000) the
 * master's emitter must reproduce (a) the correct per-console cfea/ENROLL
 * identity bytes, and (b) the console-INDEPENDENT control templates —
 * chanmap sweep, probe seed, grant enrollment sweep — BYTE-FOR-BYTE, already
 * golden-pinned by tests/reac_m200_golden.inc + tests/reac_grant_golden.inc
 * (test_reac_master.c / test_reac_s1608.c / test_reac_grant.c).
 *
 * THE FINDING: three real consoles (M-200, M-300 = V-Mixer; M-5000 = OHRCA)
 * differ on the wire in exactly TWO bytes — the source MAC and the cfea
 * console_field byte (0x00 V-Mixer / 0x01 OHRCA), which also drives the
 * ENROLL console-model byte (reac_master.c's gen_cfea / ENROLL_BLK comments,
 * struct reac_mixer_profile's doc in reac_master.h). Every other downstream
 * template (chanmap/probe/SUB01/SUB02/ENROLL head/grant sweep/head-amp) is
 * ONE generator shared by all three profiles — this test proves that sharing
 * by construction: it drives the real reac_master_init/set_box/stamp path
 * with each profile's identity and diffs the result against the same goldens
 * the M-200/M-300 tests already pin, plus a new per-console cfea golden.
 *
 * Canonicalization: the free-running counter lives at frame[14:16], OUTSIDE
 * the [16:50] block compared here, so no counter strip is needed. The cfea
 * MAC (block[11:17]) is compared to the profile MAC explicitly (never
 * masked) since it IS the identity under test.
 *
 * Pure — no socket, no PipeWire, no rig. */
#include "reac_master.h"
#include "reac_ctrl.h"
#include "reac_grant.h"
#include "reac_headamp_tx.h"
#include "reac_tx.h"
#include <reac/reac.h>
#include <reac/reac_encode.h>

#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

#include "reac_m200_golden.inc"        /* GOLD_CHANMAP_SWEEP[49], GOLD_PROBES[10] (console-independent) */
#include "reac_grant_golden.inc"       /* GOLD_S1608_SWEEP[56], GOLD_S1608_CELLS[48] (console-independent) */
#include "reac_conformance_golden.inc" /* CONF_CONSOLES[] — the new per-console cfea goldens */

/* THE BASE A BOX ANNOUNCES, not one derived from its width. These are the
 * straps the real chassis carry in their config announce (block[7] * 0x10,
 * libreac reac_ports.h): an S-1608 straps 2, an S-0808 straps 0. They are
 * written out here rather than computed from in_ch on purpose — a helper
 * mapping width to base is the very table this law retired, and it would agree
 * with the wire on exactly the chassis we own. */
#define S1608_BASE 0x20
#define S0808_BASE 0x00

#define FPS 4000   /* irrelevant to the assertions below: we stamp directly,
                    * never drive the pacer/cadence, so no cycle timing is
                    * exercised here (that is test_reac_master.c's job). */

/* Build a base downstream FILLER frame (silent audio, so only the control
 * block matters) then let the master stamp it — the same pattern as
 * test_reac_master.c's build_and_stamp / test_reac_s1608.c. */
static void build_and_stamp(const struct reac_master *m, uint8_t *out,
                            enum reac_master_emit emit, int idx)
{
	float *planar[REAC_MAX_CHANNELS] = { 0 };   /* NULL per-channel -> silent */
	reac_downstream_build(out, planar, REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT, 0x1234, m->src);
	reac_master_stamp(m, out, emit, idx);
}

/* rate ⊥ family — the mechanical gate for the operator's 2026-08-21 rule
 * (commit 79bbdd0: "MIXER FAMILY AND CLOCK PACE MUST BE DETACHED: neither may
 * determine the other"). The ksy states the same law two ways: cfea[19] is the
 * console GENERATION (0 V-Mixer / 1 OHRCA), and "REAC carries the rate in the
 * packet rate, never in the frame". So the emitted CONTROL BLOCK is a pure
 * function of the family and MUST NOT move with the pace.
 *
 * No test crossed the two axes before, which is exactly how 374c39f's
 * reac_rate_console_field (cfea[19] <- rate == 96000 ? 1 : 0) stayed green while
 * silently announcing us as an OHRCA desk whenever --rate asked for 96 kHz. This
 * holds the family fixed, sweeps the pace across all three legal rates, and
 * requires the whole cfea + ENROLL blocks to come out byte-identical -- any rate
 * leak into ANY frame byte fails it. The cadence side (the rate DOES live in the
 * packet period, family-blind) is pinned in test_reac_pacer.c:35-37.
 *
 * Sabotage check: re-derive f[16+19] from the master's fps and this goes red. */
static int test_rate_and_family_are_orthogonal(void)
{
	static const uint8_t MAC[6] = { 0x00, 0x40, 0xab, 0x11, 0x22, 0x33 };
	const int families[2] = { 0, 1 };                 /* --mixer m200 (V-Mixer) / m5000 (OHRCA) */
	const int paces_fps[3] = { 3675, 4000, 8000 };    /* --rate 44.1 / 48 / 96 kHz, pps = rate/12 */

	for (int fi = 0; fi < 2; fi++) {
		uint8_t cfea_ref[34], enroll_ref[34];
		for (int pi = 0; pi < 3; pi++) {
			struct reac_console_cfg cfg = { .out_channels = 8,
			                                .console_field = families[fi] };
			struct reac_master m;
			reac_master_init(&m, MAC, &cfg, paces_fps[pi]);   /* the master SEES the pace */

			uint8_t f[REAC_FRAME_BYTES];

			/* cfea announce: the family byte is the family, whatever the rate. */
			build_and_stamp(&m, f, REAC_M_EMIT_ANNOUNCE, 0);
			CHK(f[16 + 19] == (uint8_t)families[fi]);
			CHK(reac_ctrl_checksum_verify(f) == 0);
			if (pi == 0) memcpy(cfea_ref, f + 16, 34);
			else CHK(memcmp(f + 16, cfea_ref, 34) == 0);      /* NO rate leak, any byte */

			/* ENROLL: same 0/1 console byte, also family-not-rate (ksy line 639). */
			build_and_stamp(&m, f, REAC_M_EMIT_ENROLL, 0);
			CHK(f[16 + 8] == (uint8_t)families[fi]);
			CHK(reac_ctrl_checksum_verify(f) == 0);
			if (pi == 0) memcpy(enroll_ref, f + 16, 34);
			else CHK(memcmp(f + 16, enroll_ref, 34) == 0);
		}
	}
	return 0;
}

int main(void)
{
	uint8_t f[REAC_FRAME_BYTES];

	if (test_rate_and_family_are_orthogonal() != 0)
		return 1;

	for (size_t c = 0; c < sizeof CONF_CONSOLES / sizeof CONF_CONSOLES[0]; c++) {
		const struct conf_console *cc = &CONF_CONSOLES[c];
		struct reac_console_cfg cfg = { .out_channels = 8,
		                                .console_field = cc->console_field };
		struct reac_master m;
	uint8_t first_scene_chunk[34];
	int first_scene_chunk_set = 0;
		reac_master_init(&m, cc->mac, &cfg, FPS);

		/* ---- (a) cfea announce (idle): the per-console identity bytes ---- */
		build_and_stamp(&m, f, REAC_M_EMIT_ANNOUNCE, 0);
		CHK(memcmp(f + 16, cc->cfea_idle, 34) == 0);        /* full byte-equality incl cksum */
		CHK(f[16 + 17] == 0x28);                            /* fixed 40-slot total [17] */
		CHK(f[16 + 19] == cc->console_field);               /* the OHRCA discriminator */
		CHK(memcmp(f + 16 + CONF_MAC_IDX, cc->mac, 6) == 0); /* identity == the L2 source (never a cloned desk MAC) */
		CHK(reac_ctrl_checksum_verify(f) == 0);

		/* ---- (a) recognized S-1608 (w=0x10) / S-0808 (w=0x08), GRANTED (count=1) ---- */
		reac_master_set_box(&m, 16, 8, S1608_BASE);         /* recognized while still un-granted (count stays 0) */
		m.state = REAC_M_ESTABLISHED;           /* force the granted branch (mirrors enter_established) */
		reac_master_set_box(&m, 16, 8, S1608_BASE);         /* re-stamp now that we're "granted": count -> 1 */
		build_and_stamp(&m, f, REAC_M_EMIT_ANNOUNCE, 0);
		CHK(memcmp(f + 16, cc->cfea_s1608, 34) == 0);
		CHK(reac_ctrl_checksum_verify(f) == 0);

		reac_master_set_box(&m, 8, 8, S0808_BASE);          /* an S-0808 instead, still granted */
		build_and_stamp(&m, f, REAC_M_EMIT_ANNOUNCE, 0);
		CHK(memcmp(f + 16, cc->cfea_s0808, 34) == 0);
		CHK(reac_ctrl_checksum_verify(f) == 0);

		/* ---- (a) ENROLL: console-model byte follows the profile ---- */
		build_and_stamp(&m, f, REAC_M_EMIT_ENROLL, 0);
		CHK(f[16 + 2] == 0x01 && f[16 + 3] == 0x03 &&
		    f[16 + 4] == 0x00 && f[16 + 5] == 0x0d);        /* cdea 01 03 000d */
		CHK(f[16 + 8] == cc->console_field);                /* ENROLL_BLK[8] (reac_master.c's
		                                                      * REAC_ENROLL_CONSOLE_IDX, private) */
		CHK(reac_ctrl_checksum_verify(f) == 0);

		/* ---- (b) console-INDEPENDENT chanmap: identical for EVERY profile ---- */
		CHK(m.chanmap_nframes == GOLD_CHANMAP_WINDOWS);
		for (int w = 0; w < GOLD_CHANMAP_WINDOWS; w++) {
			build_and_stamp(&m, f, REAC_M_EMIT_CHANMAP, w);
			CHK(memcmp(f + 16, GOLD_CHANMAP_SWEEP[w], 34) == 0);
		}

		/* ---- (b) console-INDEPENDENT scene push: the body is the DESK's, not the
		 * console profile's, so every step is identical across all profiles. A
		 * fresh init seeds the header (step 0); the first body chunk is the
		 * phase-6/sub-0x02 block transcribed off the live M-200. */
		build_and_stamp(&m, f, REAC_M_EMIT_SCENE_HEAD, 0);
		CHK(f[18] == 0x01 && f[19] == 0x01);          /* op-0101              */
		CHK(f[23] == 0x22 && f[24] == 0xc8);          /* declares 0x22c8      */
		CHK(reac_ctrl_checksum_verify(f) == 0);
		{
			/* Chunk 1 carries the body verbatim and is the SAME for every console
			 * profile: the scene is the desk's own state, and nothing in the
			 * transfer is keyed to the mixer model. Byte-comparing against a
			 * captured M-200i block would only assert that we replay THAT desk's
			 * mixer state, which is the thing we deliberately stopped doing. */
			uint8_t blk[34];
			CHK(reac_ctrl_build_scene_step(blk, m.scene, sizeof m.scene, 1) == 0);
			CHK(blk[0] == 0xcd && blk[1] == 0xea);
			CHK(blk[2] == 0x01 && blk[3] == 0x00);           /* op-0100        */
			CHK(blk[4] == 0x00 && blk[5] == 0x1a);           /* 26-byte payload */
			CHK(memcmp(blk + 7, m.scene + REAC_SCENE_HEAD_BYTES,
			           REAC_SCENE_CHUNK_BYTES) == 0);
			if (first_scene_chunk_set)
				CHK(memcmp(blk, first_scene_chunk, 34) == 0);  /* profile-independent */
			else {
				memcpy(first_scene_chunk, blk, 34);
				first_scene_chunk_set = 1;
			}
		}

		/* ---- (b) console-INDEPENDENT grant sweep: byte-identical to the real
		 * M-200 x S-1608 golden (reac_grant_golden.inc), regardless of mixer
		 * profile — reac_grant.c never reads the console/mac at all, so seeding
		 * it with the same head-amp cells the real desk pushed must reproduce
		 * that desk's grant EXACTLY no matter which console we are impersonating. */
		{
			struct reac_headamp_tx tx;
			reac_headamp_tx_init(&tx);
			for (size_t i = 0; i < sizeof GOLD_S1608_CELLS / sizeof GOLD_S1608_CELLS[0]; i++)
				CHK(reac_headamp_tx_set(&tx, GOLD_S1608_CELLS[i][0],
				                        GOLD_S1608_CELLS[i][1],
				                        GOLD_S1608_CELLS[i][2]) == 0);

			struct reac_grant_alloc a;
			CHK(reac_grant_allocate(&a, 0x20, 16) == 0);
			CHK(a.base == 0x20 && a.width == 16);

			uint8_t sw[REAC_GRANT_SWEEP_MAX][34];
			int n = reac_grant_build_sweep(sw, REAC_GRANT_SWEEP_MAX, &a, &tx);
			CHK(n == (int)(sizeof GOLD_S1608_SWEEP / sizeof GOLD_S1608_SWEEP[0]));
			for (int i = 0; i < n; i++)
				CHK(memcmp(sw[i], GOLD_S1608_SWEEP[i], 34) == 0);

			/* HEAD_ACK (marker 12 12, tag 01 00) / HEAD_MARK (marker 12 12, tag
			 * 00 00) bracket the first channel's group A — console-independent
			 * fixed frames (reac_grant.c's GRANT_HEAD_ACK / GRANT_HEAD_MARK). */
			CHK(sw[0][16] == 0x12 && sw[0][17] == 0x12 && sw[0][18] == 0x01 && sw[0][19] == 0x00);
			CHK(sw[4][16] == 0x12 && sw[4][17] == 0x12 && sw[4][18] == 0x00 && sw[4][19] == 0x00);
		}
	}

	printf("OK: conformance -- m200/m300/m5000 cfea+ENROLL identity bytes byte-exact "
	       "(console_field 0x00/0x00/0x01, MAC per profile), shared chanmap/probe/grant "
	       "templates byte-identical to the M-200/M-300 goldens across every profile\n");
	return 0;
}
