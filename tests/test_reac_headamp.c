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

	/* 3c. STAMP overlay (the MASTER emit path, task #155/C.7): stamping a head-amp
	 * record over an already-built FILLER frame reproduces the SAME control block
	 * [18:50] as the captured M-200 record AND the fresh builder, while preserving
	 * the counter, tail and audio the frame already carried. This is the offline
	 * proof for the rig-gated master send. */
	{
		uint8_t frame[REAC_FRAME_BYTES];
		memset(frame, 0, sizeof frame);
		frame[14] = 0x11; frame[15] = 0x22;                 /* a counter */
		for (int i = 50; i < REAC_FRAME_BYTES - 2; i++)     /* nonzero audio */
			frame[i] = (uint8_t)(i & 0xff);
		frame[REAC_FRAME_BYTES - 2] = 0xc2;
		frame[REAC_FRAME_BYTES - 1] = 0xea;                 /* C2/EA tail */

		CHK(reac_ctrl_stamp_headamp(frame, 0x00, REAC_HEADAMP_SENS, 0x08) == 0);
		/* byte-exact vs the real M-200 ctl2.pcap record AND both checksums */
		CHK(memcmp(frame + 18, WIRE_SENS_BLOCK, 32) == 0);
		CHK(reac_ctrl_checksum_verify(frame) == 0);
		CHK(reac_ctrl_headamp_record_verify(frame) == 0);
		/* counter + tail + audio outside the block are untouched by the stamp */
		CHK(frame[14] == 0x11 && frame[15] == 0x22);
		CHK(frame[REAC_FRAME_BYTES - 2] == 0xc2 && frame[REAC_FRAME_BYTES - 1] == 0xea);
		CHK(frame[50] == (uint8_t)(50 & 0xff));
		CHK(frame[600] == (uint8_t)(600 & 0xff));
		/* the stamp equals the fresh builder's block for the same (ch,param,value) */
		{
			uint8_t built[REAC_FRAME_BYTES];
			CHK(reac_ctrl_build_headamp(built, BCAST, MASTER, 0x32d9,
			                            0x00, REAC_HEADAMP_SENS, 0x08) == REAC_FRAME_BYTES);
			CHK(memcmp(frame + 16, built + 16, 34) == 0);   /* type + block identical */
		}
		/* a bad param leaves the frame byte-for-byte untouched */
		{
			uint8_t before[REAC_FRAME_BYTES];
			memcpy(before, frame, sizeof before);
			CHK(reac_ctrl_stamp_headamp(frame, 0x00, 0x03, 0x00) == -1);
			CHK(memcmp(before, frame, sizeof before) == 0);
		}
	}

	/* 3d. RX-log helpers (task B.4): the inner-record checksum verify (a corrupt
	 * record must be dropped, not surfaced) and the param-name mapping the slave
	 * RX log uses. */
	n = reac_ctrl_build_headamp(f, BCAST, MASTER, 0x1234, 0x00, REAC_HEADAMP_PHANTOM, 0x01);
	CHK(reac_ctrl_headamp_record_verify(f) == 0);
	f[39] ^= 0xff;                                      /* corrupt the inner cksum */
	CHK(reac_ctrl_headamp_record_verify(f) != 0);
	CHK(!strcmp(reac_headamp_param_name(REAC_HEADAMP_PHANTOM), "phantom"));
	CHK(!strcmp(reac_headamp_param_name(REAC_HEADAMP_PAD), "pad"));
	CHK(!strcmp(reac_headamp_param_name(REAC_HEADAMP_SENS), "SENS"));
	CHK(!strcmp(reac_headamp_param_name(0x7f), "?"));

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
	/* THE SENS CURVE IS A TABLE, NOT A CONSTANT — and not the 1.235 dB/step I
	 * reported from a microphone either. That figure came from an acoustic source
	 * that is not stable enough for the job: the identical 32->12 command measured
	 * 24.7 dB in one session and 18.9 dB in another, while the noise floor gave
	 * -91.4 and -102.7 both times, to 0.1 dB.
	 *
	 * The authority is the box's own 56-entry table at 0x0c0327a0, and the scale
	 * comes from the floor, which tracks gain exactly inside a stage and
	 * reproduces to 0.03 dB. Measured on an S-0808:
	 *
	 *   stage 2 (idx  8..23)  0.90 dB/step      break 8   no gain step
	 *   stage 1 (idx 24..39)  0.95 dB/step      break 24  no gain step
	 *   stage 0 (idx 40..55)  0.98 dB/step      break 40  no gain step
	 *   stage 3 (idx  0.. 7)  unmeasured, assumed 0.90 (its floor is under the
	 *                         converter's, so it cannot be read this way)
	 *
	 * Span 48.75 dB, not 55. The breaks were confirmed at exactly the indices the
	 * firmware table predicts, and they carry no gain step: across 23->24 the
	 * signal is flat while the floor drops 6.06 dB, so what changes there is the
	 * noise figure, not the gain.
	 *
	 * Changing the scale is a contract change — the conversion runs both ways,
	 * reac_slave.c derives virtual preamp gain from it, and openmixer publishes
	 * sensDbu — so these assertions exist to make the next change deliberate. */
	/* Endpoints. The span is 48.75 dB across the 56 steps, not the 55 dB a flat
	 * 1 dB per step implies — see the header for how each stage was measured. */
	CHK(reac_headamp_sens_cdb(0x00, 0) == -1000);
	CHK(reac_headamp_sens_cdb(0x37, 0) == -1000 - 4875);
	CHK(reac_headamp_sens_cdb(0x00, 1) ==  1000);
	CHK(reac_headamp_sens_cdb(0x37, 1) ==  1000 - 4875);
	CHK(reac_headamp_sens_db(0x00, 0) == -10);
	CHK(reac_headamp_sens_db(0x37, 0) == -59);   /* -58.75, rounded */
	CHK(reac_headamp_sens_value_cdb(-1000, 0) == 0x00);
	CHK(reac_headamp_sens_value_cdb(-1000 - 4875, 0) == 0x37);

	/* THE CURVE IS NOT A LINE, and this is where that is pinned. Steps inside a
	 * stage differ by stage, and the three stage breaks carry NO gain step at
	 * all. A single constant cannot express this, which is the entire reason the
	 * conversion is a table. */
	CHK(reac_headamp_sens_cdb(9, 0)  - reac_headamp_sens_cdb(10, 0) == 90);  /* stage 2 */
	CHK(reac_headamp_sens_cdb(25, 0) - reac_headamp_sens_cdb(26, 0) == 95);  /* stage 1 */
	CHK(reac_headamp_sens_cdb(41, 0) - reac_headamp_sens_cdb(42, 0) == 98);  /* stage 0 */
	CHK(reac_headamp_sens_cdb(7, 0)  == reac_headamp_sens_cdb(8, 0));        /* break */
	CHK(reac_headamp_sens_cdb(23, 0) == reac_headamp_sens_cdb(24, 0));       /* break */
	CHK(reac_headamp_sens_cdb(39, 0) == reac_headamp_sens_cdb(40, 0));       /* break */

	/* ROUND TRIP, in centi-dB where it is exact: step -> dB -> step is the
	 * identity everywhere except the three lower twins of a break, which the
	 * reverse lookup deliberately promotes to their quieter partner (same gain,
	 * 6.06 dB less noise). Asserting the exception rather than excusing it. */
	for (int pad = 0; pad <= 1; pad++) {
		for (int v = 0; v <= REAC_HEADAMP_SENS_MAX; v++) {
			int back = reac_headamp_sens_value_cdb(
				reac_headamp_sens_cdb((uint8_t)v, pad), pad);
			int expect = (v == 7 || v == 23 || v == 39) ? v + 1 : v;
			CHK(back == expect);
		}
	}

	/* The whole-dB pair is LOSSY and must not be trusted to round-trip: the
	 * device's steps are all under 1 dB, so neighbouring steps collide on one
	 * integer. Pinned as a known limitation so nobody re-derives it as a bug. */
	{
		int collisions = 0;
		for (int v = 0; v < REAC_HEADAMP_SENS_MAX; v++)
			if (reac_headamp_sens_db((uint8_t)v, 0) ==
			    reac_headamp_sens_db((uint8_t)(v + 1), 0))
				collisions++;
		CHK(collisions > 0);
	}

	CHK(reac_headamp_sens_value(99, 0) == 0x00);      /* hotter than min gain */
	CHK(reac_headamp_sens_value(-99, 0) == 0x37);     /* below max gain */     /* below max gain */

	printf("OK: head-amp record byte-exact vs the M-200 capture (inner 0x80 / "
	       "outer sum-0), TAG dispatch grant-safe, SENS codec is the firmware's\n"
	       "56-entry table: per-stage steps, no gain at the three breaks, and a\n"
	       "centi-dB round trip that is the identity bar the promoted twins\n");
	return 0;
}
