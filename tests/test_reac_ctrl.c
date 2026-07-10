// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_ctrl builders/parser/checksum, validated against the real wire WITHOUT
 * embedding any rig MAC: the box-heartbeat control block is MAC-independent, so
 * its checksum byte (0x7a, observed on wire in reac-captures) is a fixed
 * cross-check of the checksum algorithm. Geometry (628 B box width, 00 7a
 * descriptor, C2 EA trailer) is ground-truthed in REAC-CONNECTION-FSM.md. */
#include "reac_ctrl.h"
#include "reac_upstream.h"
#include <reac/reac.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static const uint8_t MASTER[6] = { 0x00, 0x40, 0xab, 0x11, 0x22, 0x33 }; /* stand-in */
static const uint8_t SRC[6]    = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0xf6 }; /* our stand-in */

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

int main(void)
{
	uint8_t f[1536];

	/* 1. box heartbeat: cdea 01 03 0001 81, 628 B, checksum == 0x7a (wire value) */
	size_t n = reac_ctrl_build_box_hb(f, MASTER, SRC, 0x1234);
	CHK(n == 628);
	CHK(f[16] == 0xcd && f[17] == 0xea);
	CHK(f[18] == 0x01 && f[19] == 0x03 && f[20] == 0x00 && f[21] == 0x01 && f[22] == 0x81);
	CHK(f[626] == 0xc2 && f[627] == 0xea);
	CHK(reac_ctrl_checksum_verify(f) == 0);
	CHK(f[49] == 0x7a);   /* <-- cross-check vs the real captured box heartbeat */

	struct reac_ctrl_parsed p;
	CHK(reac_ctrl_parse(f, n, &p) == REAC_CTRL_BOX_HB);
	CHK(p.counter == 0x1234 && p.op0 == 1 && p.op1 == 3 && p.op_len == 1 && p.sel == 0x81);
	CHK(!p.is_broadcast && memcmp(p.src, SRC, 6) == 0 && memcmp(p.dst, MASTER, 6) == 0);

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
	reac_ctrl_build_coldconnect(f, MASTER, SRC, 1);
	CHK(reac_ctrl_checksum_verify(f) == 0 && f[18] == 0x04 && f[19] == 0x03);

	/* 4. 8-channel box width -> 340 B */
	n = reac_ctrl_build_upstream_filler(f, MASTER, SRC, 1, 8, NULL, 12);
	CHK(n == 340);

	printf("OK: reac_ctrl builders byte-faithful (box-hb checksum 0x7a matches wire), "
	       "parser + descriptor + audio round-trip clean\n");
	return 0;
}
