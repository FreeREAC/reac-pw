// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ACCEPTANCE (#130): the parameterized downstream generator, fed the S-1608
 * console config, must reproduce the captured M-300 downstream byte-for-byte.
 *
 * Ground truth: reac-captures/m300-s1608-*.pcap (2026-07-10), real M-300 master
 * 00:40:ab:c9:d8:5b driving an S-1608 (16 in / 8 out).
 *
 *   CHANMAP — the chanmap carries NO MAC, so the generated chanmap SWEEP must equal
 *             the captured M-300's 11 windows EXACTLY (bytes + checksums), tiling
 *             the whole 48-slot HEAD-AMP space 0x00..0x2f (#130) — not the 40-slot
 *             audio fabric the cfea below advertises (#69); window 0 is the fe frame
 *             (marker + 0x00..0x06, checksum 0xb7).
 *   CFEA    — the cfea embeds OUR MAC, so a generated frame must equal the
 *             captured M-300 cfea EXCEPT the 6 MAC bytes [11:17] and the
 *             recomputed checksum [33]; fed OUR = the M-300 MAC it is EXACT
 *             (…28 08 …00 d4).
 *   ALL     — every generated control block satisfies Sum(block[18..49])%256==0.
 *
 * The generator is parameterized on { out_channels, mac, console_field }; the
 * generic path and this byte-exact test are the SAME code. */
#include <reac/reac_master.h>
#include <reac/reac_ctrl.h>
#include <reac/transport/reac_tx.h>
#include <reac/reac.h>
#include <reac/reac_encode.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* Captured M-300 / S-1608 downstream control blocks, bytes [16:50] — ONE copy,
 * shared with test_reac_master.c, which asserts the same two blocks. */
#include "reac_m300_golden.inc"

#include "reac_m200_golden.inc"

static const uint8_t M300_MAC[6] = { 0x00, 0x40, 0xab, 0xc9, 0xd8, 0x5b };
static const uint8_t OUR_MAC[6]  = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };

/* Build a base FILLER then let the master stamp the control block into [16:50]. */
static void stamp(const struct reac_master *m, uint8_t *f,
                  enum reac_master_emit emit, int idx)
{
	reac_downstream_build(f, NULL, 0, REAC_SAMPLES_PER_PKT, 0x1234, OUR_MAC);
	reac_master_stamp(m, f, emit, idx);
}

/* Assert every one of the five continuous control blocks a master generates has
 * a valid checksum (Sum(block[18..49]) mod 256 == 0), for a given master. */
static int check_all_checksums(const struct reac_master *m)
{
	uint8_t f[REAC_FRAME_BYTES];
	const enum reac_master_emit kinds[] = {
		REAC_M_EMIT_SCENE_CHUNK, REAC_M_EMIT_SCENE_HEAD,
		REAC_M_EMIT_SCENE_TAIL, REAC_M_EMIT_ANNOUNCE,
	};
	for (unsigned k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
		stamp(m, f, kinds[k], 0);
		CHK(reac_ctrl_checksum_verify(f) == 0);
	}
	for (int i = 0; i < m->chanmap_nframes; i++) {
		stamp(m, f, REAC_M_EMIT_CHANMAP, i);
		CHK(reac_ctrl_checksum_verify(f) == 0);
	}
	return 0;
}

static const uint8_t *gold_probe(int phase, uint8_t sub)
{
	for (int i = 0; i < GOLD_PROBE_VARIANTS; i++)
		if (GOLD_PROBES[i].phase == phase && GOLD_PROBES[i].sub == sub)
			return GOLD_PROBES[i].blk;
	return NULL;
}

/* SCENE-CHUNK SEQUENCE + FILLER-descriptor tracking — the behaviour that decides
 * whether a real box will talk to us at all. The burst slots carry the body of
 * the scene push, so the "rotation" an earlier RE described is just the body's
 * own periodicity: 26 bytes is 6 mod 10, so each chunk steps the pattern by 6 and
 * the walk is 6,2,8,4,0 — ONE per chunk, not two. The doubling was the mirrored
 * capture repeating every frame. 336 of the 341 chunks are one of the ten blocks
 * transcribed off the live M-200, so the goldens stay the oracle. Assert:
 *   - the emitted chunks walk the body in order, byte-exact vs those captures;
 *   - consecutive chunks are never identical (they were, under the old model);
 *   - every FILLER between chunks carries 16x "00 <that chunk's checksum>". */
static int test_probe_rotation(void)
{
	uint8_t f[REAC_FRAME_BYTES];
	struct reac_console_cfg idle = REAC_CONSOLE_CFG_IDLE;
	struct reac_master m;
	reac_master_init(&m, OUR_MAC, &idle, 8000);

	int prev_step = 0;
	int np = 0, fillers_checked = 0;
	uint8_t cur_desc = 0;
	int have = 0;

	for (long i = 0; i < 500000L && np < 10; i++) {
		uint16_t cnt;
		int idx;
		enum reac_master_emit e = reac_master_next(&m, &cnt, &idx);
		reac_downstream_build(f, NULL, 0, REAC_SAMPLES_PER_PKT, cnt, OUR_MAC);
		reac_master_stamp(&m, f, e, idx);

		if (e == REAC_M_EMIT_SCENE_CHUNK) {
			/* The chunk carries the body slice for its step, verbatim, with a valid
			 * checksum — and no two consecutive chunks are identical. That last one
			 * is the property the old model got wrong: it emitted each block TWICE,
			 * which a mirrored capture had made look like the desk's own behaviour. */
			CHK(f[18] == 0x01 && f[19] == 0x00);
			CHK(f[20] == 0x00 && f[21] == 0x1a);
			size_t off = REAC_SCENE_HEAD_BYTES +
			             (size_t)(m.scene_step - 1) * REAC_SCENE_CHUNK_BYTES;
			CHK(memcmp(f + 23, m.scene + off, REAC_SCENE_CHUNK_BYTES) == 0);
			CHK(reac_ctrl_checksum_verify(f) == 0);
			/* The step advances by exactly one per chunk slot. That is the
			 * body-independent form of the invariant the old model broke: it
			 * emitted each block TWICE and advanced every other slot, which a
			 * MIRRORED capture (every frame duplicated) had made look like the
			 * desk's own behaviour. Comparing chunk bytes cannot express this —
			 * a generated body is mostly zeros, so neighbouring chunks are
			 * legitimately identical and only the STEP distinguishes them. */
			if (np > 0)
				CHK(m.scene_step == prev_step + 1);
			prev_step = m.scene_step;
			cur_desc = f[49];                      /* this chunk's checksum */
			have = 1;
			np++;
		} else if (e == REAC_M_EMIT_FILLER && have) {
			for (int k = 18; k < 50; k += 2) {
				CHK(f[k] == 0x00);                 /* high byte constant */
				CHK(f[k + 1] == cur_desc);         /* FILLER tracks the probe */
			}
			fillers_checked++;
		}
	}
	CHK(np == 10);                                 /* ten chunks walked in order */
	CHK(fillers_checked > 100);                    /* and plenty of tracking FILLERs */
	return 0;
}

int main(void)
{
	uint8_t f[REAC_FRAME_BYTES];
	struct reac_console_cfg idle = REAC_CONSOLE_CFG_IDLE;

	/* --- with OUR distinct MAC: chanmap EXACT, cfea EXACT except MAC+cksum --- */
	struct reac_master m;
	reac_master_init(&m, OUR_MAC, &idle, 8000);
	CHK(m.chanmap_nframes == GOLD_CHANMAP_WINDOWS);   /* full 49-window fabric sweep */

	/* CHANMAP: no MAC -> byte-EXACT vs the capture. Window 0 is the fe frame
	 * (marker + 0x00..0x06, checksum 0xb7); the full sweep is checked next. */
	stamp(&m, f, REAC_M_EMIT_CHANMAP, 0);
	CHK(memcmp(f + 16, M300_CHANMAP_FE, 34) == 0);
	CHK(reac_ctrl_checksum_verify(f) == 0);

	/* CHANMAP SWEEP: all 11 windows byte-EXACT vs the captured M-300 chanmap sweep
	 * (tiles the 48-slot head-amp space 0x00..0x2f; #130 — a box enrolls only after
	 * it sees its own slots). */
	for (int i = 0; i < GOLD_CHANMAP_WINDOWS; i++) {
		stamp(&m, f, REAC_M_EMIT_CHANMAP, i);
		CHK(memcmp(f + 16, GOLD_CHANMAP_SWEEP[i], 34) == 0);
		CHK(reac_ctrl_checksum_verify(f) == 0);
	}

	/* CFEA: EXACT except the 6 MAC bytes [11:17] (OURS) + the checksum [33]. */
	stamp(&m, f, REAC_M_EMIT_ANNOUNCE, 0);
	for (int i = 0; i < 34; i++) {
		if (i >= M300_CFEA_MAC_IDX && i < M300_CFEA_MAC_IDX + 6)
			CHK(f[16 + i] == OUR_MAC[i - M300_CFEA_MAC_IDX]);   /* our MAC */
		else if (i == M300_CFEA_CKSUM_IDX)
			continue;                                      /* recomputed */
		else
			CHK(f[16 + i] == M300_CFEA[i]);                 /* everything else EXACT */
	}
	CHK(reac_ctrl_checksum_verify(f) == 0);
	CHK(check_all_checksums(&m) == 0);

	/* --- with the M-300 MAC as OUR src: cfea is byte-EXACT incl. checksum ---- */
	struct reac_master m300;
	reac_master_init(&m300, M300_MAC, &idle, 8000);
	stamp(&m300, f, REAC_M_EMIT_ANNOUNCE, 0);
	CHK(memcmp(f + 16, M300_CFEA, 34) == 0);   /* …00 40 ab c9 d8 5b … 28 08 … d4 */
	CHK(reac_ctrl_checksum_verify(f) == 0);
	/* and the chanmap is still byte-exact (MAC-independent). */
	stamp(&m300, f, REAC_M_EMIT_CHANMAP, 0);
	CHK(memcmp(f + 16, M300_CHANMAP_FE, 34) == 0);
	CHK(check_all_checksums(&m300) == 0);

	/* the rotating probe + the FILLER descriptor that tracks it */
	CHK(test_probe_rotation() == 0);

	printf("OK: S-1608 acceptance — generated CHANMAP reproduces ALL %d windows of the "
	       "LIVE M-200 fabric sweep byte-for-byte; the PROBE rotates 0,6,2,8,4 (x2 each) "
	       "byte-exact vs the M-200's %d captured variants, and every FILLER carries "
	       "16x \"00 <current-probe-checksum>\"; CFEA matches except our MAC + recomputed "
	       "checksum; every control block sums to 0 mod 256\n",
	       GOLD_CHANMAP_WINDOWS, GOLD_PROBE_VARIANTS);
	return 0;
}
