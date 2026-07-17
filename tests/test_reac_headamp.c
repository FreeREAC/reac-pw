// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Head-amp source control (op 04 03, record TAG 01 01), ground-truthed on a
 * live M-200 + S-0808 + S-1608 (reac-captures/m200-headamp-re/DECODE.md):
 * builder bytes vs the worked ch1-phantom example AND a REAL captured SENS
 * knob-turn record (ctl2.pcap), the parser TAG dispatch (a preamp knob-turn is
 * NOT a grant), and the pad-relative SENS dB codec. MAC-free like the other
 * ctrl tests: the control block [18:50] is MAC-independent. */
#include "reac_ctrl.h"
#include <reac/reac.h>
#include <stdio.h>
#include <string.h>

static const uint8_t MASTER[6] = { 0x00, 0x40, 0xab, 0x11, 0x22, 0x33 }; /* stand-in */
static const uint8_t SRC[6]    = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0xf6 }; /* our stand-in */
static const uint8_t BCAST[6]  = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* A REAL head-amp record off the wire: control block [18:50] of a broadcast
 * cdea 04 03 0013 frame from a live M-200 SENS knob-turn (counter 0x32d9,
 * reac-captures/m200-headamp-re/ctl2.pcap, 2026-07-17). Record after the 12 12
 * marker: TAG 01 01, CH 0x00, PARAM 0x02 (SENS), VALUE 0x08, inner cks 0x74
 * (00+02+08+74 == 0x7e; full record 01+01+00+02+08+74 == 0x80), f7 terminator,
 * outer block checksum 0x02 at [49] (block sums to 0 mod 256). */
static const uint8_t WIRE_SENS_BLOCK[32] = {
	0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe,
	0x0e, 0xf0, 0x41, 0x0a, 0x00, 0x00, 0x12, 0x12,
	0x01, 0x01, 0x00, 0x02, 0x08, 0x74, 0xf7, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 };

int main(void)
{
	uint8_t f[1536];
	struct reac_ctrl_parsed p;

	/* 1. worked example: ch1 phantom ON (S-0808, model_base 0x00 -> wire CH 0x00).
	 * Exact bytes: TAG 01 01, DATA 00 00 01, inner cks 0x7d, outer cks 0x02. */
	size_t n = reac_ctrl_build_headamp(f, BCAST, MASTER, 0x1234,
	                                   0x00, REAC_HEADAMP_PHANTOM, 0x01);
	CHK(n == REAC_FRAME_BYTES);                       /* master/downstream width */
	CHK(f[16] == 0xcd && f[17] == 0xea);
	CHK(f[18] == 0x04 && f[19] == 0x03 && f[20] == 0x00 && f[21] == 0x13);
	CHK(f[22] == 0x00 && f[23] == 0x02 && f[24] == 0x00 && f[25] == 0xfe);
	CHK(f[26] == 0x0e);                               /* preamble echo: oplen - 5 */
	CHK(f[27] == 0xf0 && f[28] == 0x41 && f[29] == 0x0a && f[30] == 0x00 && f[31] == 0x00);
	CHK(f[32] == 0x12 && f[33] == 0x12);              /* record marker */
	CHK(f[34] == 0x01 && f[35] == 0x01);              /* TAG 01 01 */
	CHK(f[36] == 0x00 && f[37] == 0x00 && f[38] == 0x01);
	CHK(f[39] == 0x7d);                               /* inner cks (worked example) */
	CHK(f[40] == 0xf7);
	CHK(f[n - 2] == 0xc2 && f[n - 1] == 0xea);
	/* both checksum rules, computed not assumed: record TAG..CKSUM sums to 0x80,
	 * CH+PARAM+VALUE+CKSUM == 0x7e, and the 32-byte block sums to 0 mod 256. */
	{
		unsigned rec = 0, blk = 0;
		for (int i = 34; i <= 39; i++) rec += f[i];
		for (int i = 18; i < 50; i++)  blk += f[i];
		CHK((rec & 0xff) == 0x80);
		CHK(((f[36] + f[37] + f[38] + f[39]) & 0xff) == 0x7e);
		CHK((blk & 0xff) == 0);
	}
	CHK(reac_ctrl_checksum_verify(f) == 0);
	CHK(f[49] == 0x02);

	/* 2. round-trip: build -> parse -> HEADAMP with ch/param/value recovered */
	CHK(reac_ctrl_parse(f, n, &p) == REAC_CTRL_HEADAMP);
	CHK(p.ch == 0x00 && p.param == REAC_HEADAMP_PHANTOM && p.value == 0x01);
	CHK(p.counter == 0x1234 && p.is_broadcast);
	CHK(p.op0 == 0x04 && p.op1 == 0x03 && p.op_len == 0x0013);

	/* 3. BYTE-COMPARE vs the real M-200 record: same ch/param/value must
	 * reproduce the captured control block [18:50] exactly. */
	n = reac_ctrl_build_headamp(f, BCAST, MASTER, 0x32d9,
	                            0x00, REAC_HEADAMP_SENS, 0x08);
	CHK(n == REAC_FRAME_BYTES);
	if (memcmp(f + 18, WIRE_SENS_BLOCK, 32) != 0) {
		fprintf(stderr, "FAIL: block [18:50] differs from ctl2.pcap capture:\n"
		        "  off built wire\n");
		for (int i = 0; i < 32; i++)
			if (f[18 + i] != WIRE_SENS_BLOCK[i])
				fprintf(stderr, "  [%2d] %02x   %02x\n",
				        i, f[18 + i], WIRE_SENS_BLOCK[i]);
		return 1;
	}
	CHK(reac_ctrl_parse(f, n, &p) == REAC_CTRL_HEADAMP);
	CHK(p.ch == 0x00 && p.param == REAC_HEADAMP_SENS && p.value == 0x08);

	/* 3b. S-1608 wire channels (model_base 0x20): ch1 = 0x20, ch16 = 0x2f;
	 * the general record-sum rule holds at every CH. */
	n = reac_ctrl_build_headamp(f, BCAST, MASTER, 1, 0x2f, REAC_HEADAMP_PAD, 0x01);
	CHK(n == REAC_FRAME_BYTES && reac_ctrl_checksum_verify(f) == 0);
	{
		unsigned rec = 0;
		for (int i = 34; i <= 39; i++) rec += f[i];
		CHK((rec & 0xff) == 0x80);
	}
	CHK(reac_ctrl_parse(f, n, &p) == REAC_CTRL_HEADAMP);
	CHK(p.ch == 0x2f && p.param == REAC_HEADAMP_PAD && p.value == 0x01);

	/* 4. TAG dispatch (the #33 fix): the cold-connect records still parse as
	 * GRANT — 0014 carries TAG 01 00, 0013 carries TAG 03 02 — while only
	 * TAG 01 01 is HEADAMP. A live M-200 emits ~628 head-amp records per 14
	 * grants; a joining slave must NOT read a knob-turn as its grant. */
	n = reac_ctrl_build_coldconnect(f, MASTER, SRC, 7, 16, NULL, 12);
	CHK(f[34] == 0x01 && f[35] == 0x00);              /* TAG 01 00 */
	CHK(reac_ctrl_parse(f, n, &p) == REAC_CTRL_GRANT);
	n = reac_ctrl_build_coldconnect_0013(f, MASTER, SRC, 7, 16, NULL, 12);
	CHK(f[34] == 0x03 && f[35] == 0x02);              /* TAG 03 02 */
	CHK(reac_ctrl_parse(f, n, &p) == REAC_CTRL_GRANT);
	n = reac_ctrl_build_coldconnect_0016(f, MASTER, SRC, 7, 16, NULL, 12);
	CHK(f[34] == 0x05 && f[35] == 0x00);              /* TAG 05 00 (identity) */
	CHK(reac_ctrl_parse(f, n, &p) == REAC_CTRL_GRANT);

	/* 5. bad args are rejected: unknown param, out-of-range values */
	CHK(reac_ctrl_build_headamp(f, BCAST, MASTER, 1, 0, 0x03, 0x00) == 0);
	CHK(reac_ctrl_build_headamp(f, BCAST, MASTER, 1, 0, REAC_HEADAMP_PHANTOM, 0x02) == 0);
	CHK(reac_ctrl_build_headamp(f, BCAST, MASTER, 1, 0, REAC_HEADAMP_PAD, 0x02) == 0);
	CHK(reac_ctrl_build_headamp(f, BCAST, MASTER, 1, 0, REAC_HEADAMP_SENS, 0x38) == 0);

	/* 6. SENS dB codec at all anchors (dB = -10 - value + (pad ? 20 : 0)):
	 * pad off 0x00 = -10 dBu .. 0x37 = -65 dBu; pad on 0x00 = +10 .. 0x37 = -45. */
	CHK(reac_headamp_sens_db(0x00, 0) == -10);
	CHK(reac_headamp_sens_db(0x37, 0) == -65);
	CHK(reac_headamp_sens_db(0x00, 1) == 10);
	CHK(reac_headamp_sens_db(0x37, 1) == -45);
	CHK(reac_headamp_sens_value(-10, 0) == 0x00);
	CHK(reac_headamp_sens_value(-65, 0) == 0x37);
	CHK(reac_headamp_sens_value(10, 1) == 0x00);
	CHK(reac_headamp_sens_value(-45, 1) == 0x37);
	/* every value round-trips, both pad states; out-of-range dB clamps */
	for (int pad = 0; pad <= 1; pad++)
		for (int v = 0; v <= REAC_HEADAMP_SENS_MAX; v++)
			CHK(reac_headamp_sens_value(reac_headamp_sens_db((uint8_t)v, pad), pad) == v);
	CHK(reac_headamp_sens_value(99, 0) == 0x00);      /* hotter than min gain */
	CHK(reac_headamp_sens_value(-99, 0) == 0x37);     /* below max gain */

	printf("OK: head-amp record byte-exact vs the M-200 capture (inner 0x80 / "
	       "outer sum-0), TAG dispatch grant-safe, SENS dB codec anchored\n");
	return 0;
}
