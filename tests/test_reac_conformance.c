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

int main(void)
{
	uint8_t f[REAC_FRAME_BYTES];

	for (size_t c = 0; c < sizeof CONF_CONSOLES / sizeof CONF_CONSOLES[0]; c++) {
		const struct conf_console *cc = &CONF_CONSOLES[c];
		struct reac_console_cfg cfg = { .out_channels = 8,
		                                .console_field = cc->console_field };
		struct reac_master m;
		reac_master_init(&m, cc->mac, &cfg, FPS);

		/* ---- (a) cfea announce (idle): the per-console identity bytes ---- */
		build_and_stamp(&m, f, REAC_M_EMIT_ANNOUNCE, 0);
		CHK(memcmp(f + 16, cc->cfea_idle, 34) == 0);        /* full byte-equality incl cksum */
		CHK(f[16 + 17] == 0x28);                            /* fixed 40-slot total [17] */
		CHK(f[16 + 19] == cc->console_field);               /* the OHRCA discriminator */
		CHK(memcmp(f + 16 + CONF_MAC_IDX, cc->mac, 6) == 0); /* identity == the L2 source (never a cloned desk MAC) */
		CHK(reac_ctrl_checksum_verify(f) == 0);

		/* ---- (a) recognized S-1608 (w=0x10) / S-0808 (w=0x08), GRANTED (count=1) ---- */
		reac_master_set_box(&m, 16, 8);         /* recognized while still un-granted (count stays 0) */
		m.state = REAC_M_ESTABLISHED;           /* force the granted branch (mirrors enter_established) */
		reac_master_set_box(&m, 16, 8);         /* re-stamp now that we're "granted": count -> 1 */
		build_and_stamp(&m, f, REAC_M_EMIT_ANNOUNCE, 0);
		CHK(memcmp(f + 16, cc->cfea_s1608, 34) == 0);
		CHK(reac_ctrl_checksum_verify(f) == 0);

		reac_master_set_box(&m, 8, 8);          /* an S-0808 instead, still granted */
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

		/* ---- (b) console-INDEPENDENT probe seed: fresh-init phase 0 / sub 0x02 ---- */
		build_and_stamp(&m, f, REAC_M_EMIT_PROBE, 0);
		CHK(memcmp(f + 16, GOLD_PROBES[0].blk, 34) == 0);
		CHK(reac_ctrl_checksum_verify(f) == 0);

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
			CHK(reac_grant_allocate(&a, 16) == 0);
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
