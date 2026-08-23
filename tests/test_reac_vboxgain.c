// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Virtual-stagebox head-amp -> INPUT GAIN (task #202). A real box applies the
 * console's per-channel SENS/PAD to its mic preamp before the A/D; a virtual box
 * has no preamp, so it must apply the equivalent DIGITAL gain to the audio it
 * returns upstream, or the master's head-amp is inert and the box can't be used to
 * test a session. These offline tests pin the gain MODEL (direction / 1 dB step /
 * pad = 20 dB less / default unity), the WIRE-CH -> input-index mapping, and the
 * RT-path multiply-only apply. No socket, no live rig. */
#include "reac_slave.h"
#include "reac_ctrl.h"

#include <reac/reac.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)
/* relative tolerance for float gain comparisons */
static int approx(float a, float b) { return fabsf(a - b) <= 1e-4f * (1.0f + fabsf(b)); }

static const uint8_t SRC[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0xf6 };

/* Build a bare parsed head-amp record — reac_slave_headamp_rx only reads
 * ch/param/value, so no real frame is needed. */
static struct reac_ctrl_parsed ha_rec(uint8_t ch, uint8_t param, uint8_t value)
{
	struct reac_ctrl_parsed p;
	memset(&p, 0, sizeof p);
	p.kind = REAC_CTRL_HEADAMP;
	p.ch = ch;
	p.param = param;
	p.value = value;
	return p;
}

static void slave16(struct reac_slave *s)
{
	struct reac_slave_cfg cfg = { .ifname = NULL, .box_channels = 16,
	                              .sample_rate = 48000, .src_mac = SRC };
	reac_slave_fsm_init(s, &cfg);
}

int main(void)
{
	const float STEP = powf(10.0f, 1.0f / 20.0f);   /* +1 dB in linear amplitude */

	/* (a) DIRECTION: a higher SENS value is a more sensitive input (lower dBu) and
	 * so MORE gain. NON-DECREASING, not strictly increasing: the box's own step
	 * table breaks into four coarse stages, and at each break (7->8, 23->24,
	 * 39->40) the coarse stage changes while the fine code resets, so gain is
	 * unchanged. Three pairs of steps therefore deliver identical gain and differ
	 * only in noise figure. */
	for (int v = 0; v < REAC_HEADAMP_SENS_MAX; v++)
		CHK(reac_slave_headamp_gain((uint8_t)(v + 1), 0) >=
		    reac_slave_headamp_gain((uint8_t)v, 0));
	for (int v = 0; v < REAC_HEADAMP_SENS_MAX; v++) {
		int flat = (v == 7 || v == 23 || v == 39);
		int equal = approx(reac_slave_headamp_gain((uint8_t)(v + 1), 0),
		                   reac_slave_headamp_gain((uint8_t)v, 0));
		CHK(flat ? equal : !equal);      /* flat EXACTLY at the three breaks */
	}

	/* (b) THE STEP IS PER-STAGE, NOT A CONSTANT. 0.90 dB inside stage 2, 0.95 in
	 * stage 1, 0.98 in stage 0 — measured on the metal off the preamp's own noise
	 * floor, which tracks gain exactly inside a stage. The old assertion here was
	 * a flat 1 dB per step, which is what the firmware's table refutes. */
	for (int v = 0; v < REAC_HEADAMP_SENS_MAX; v++) {
		if (v == 7 || v == 23 || v == 39)
			continue;                    /* the breaks, covered above */
		int cdb = reac_headamp_sens_cdb((uint8_t)v, 0) -
		          reac_headamp_sens_cdb((uint8_t)(v + 1), 0);
		float step = powf(10.0f, (float)cdb / 2000.0f);
		CHK(approx(reac_slave_headamp_gain((uint8_t)(v + 1), 0),
		           reac_slave_headamp_gain((uint8_t)v, 0) * step));
	}


	/* (c) PAD ON = 20 dB LESS gain (a 10x smaller linear multiplier) at the same
	 * SENS value. */
	for (int v = 0; v <= REAC_HEADAMP_SENS_MAX; v++)
		CHK(approx(reac_slave_headamp_gain((uint8_t)v, 1),
		           reac_slave_headamp_gain((uint8_t)v, 0) * 0.1f));

	/* Model anchor, on the measured curve rather than a flat 1 dB per step. Step
	 * 0x08 is the FIRST entry of stage 2, so it carries stage 3's seven steps of
	 * 0.90 dB and nothing for the break: 6.30 dB of gain, sens -16.30 dBu. The old
	 * anchor here said -18, which is what a straight line predicts and the box does
	 * not do. */
	CHK(reac_headamp_sens_cdb(0x08, 0) == -1630);
	CHK(reac_headamp_sens_cdb(0x07, 0) == -1630);        /* its twin, same gain */
	CHK(approx(reac_slave_headamp_gain(0x08, 0), powf(10.0f, 16.30f / 20.0f)));

	/* (d) DEFAULT UNITY: a fresh slave has every input's gain at 1.0 until the
	 * master sends anything, and applying an all-unity table is a byte no-op. */
	{
		struct reac_slave s;
		slave16(&s);
		CHK(s.ch_base == 0x20);                 /* 16-input S-1608 sits at 0x20 */
		for (int c = 0; c < REAC_MAX_CHANNELS; c++)
			CHK(s.ha_gain[c] == 1.0f);

		float buf[16][REAC_SAMPLES_PER_PKT];
		float *planar[16];
		for (int c = 0; c < 16; c++) {
			planar[c] = buf[c];
			for (int i = 0; i < REAC_SAMPLES_PER_PKT; i++)
				buf[c][i] = 0.25f * (float)(c + 1);
		}
		reac_slave_apply_input_gain(planar, 16, REAC_SAMPLES_PER_PKT, s.ha_gain);
		for (int c = 0; c < 16; c++)
			for (int i = 0; i < REAC_SAMPLES_PER_PKT; i++)
				CHK(buf[c][i] == 0.25f * (float)(c + 1));   /* unchanged */
	}

	/* WIRE-CH -> input-index mapping + out-of-box rejection (16-input box, base
	 * 0x20). ch 0x20 -> input 0, ch 0x2f -> input 15; anything below 0x20 or at/
	 * above 0x30 is another box and must be ignored. */
	{
		struct reac_slave s;
		slave16(&s);
		struct reac_ctrl_parsed p;
		p = ha_rec(0x20, REAC_HEADAMP_SENS, 0x10); CHK(reac_slave_headamp_rx(&s, &p) == 0);
		p = ha_rec(0x2f, REAC_HEADAMP_SENS, 0x10); CHK(reac_slave_headamp_rx(&s, &p) == 15);
		p = ha_rec(0x00, REAC_HEADAMP_SENS, 0x10); CHK(reac_slave_headamp_rx(&s, &p) == -1);
		p = ha_rec(0x1f, REAC_HEADAMP_SENS, 0x10); CHK(reac_slave_headamp_rx(&s, &p) == -1);
		p = ha_rec(0x30, REAC_HEADAMP_SENS, 0x10); CHK(reac_slave_headamp_rx(&s, &p) == -1);
		/* an unknown param on an in-range channel is ignored too */
		p = ha_rec(0x20, 0x7f, 0x00);              CHK(reac_slave_headamp_rx(&s, &p) == -1);
	}

	/* An 8-input box (S-0808) sits at base 0x00: ch 0x00 -> input 0, ch 0x07 ->
	 * input 7, ch 0x08 -> another box (ignored). */
	{
		struct reac_slave s;
		struct reac_slave_cfg cfg = { .ifname = NULL, .box_channels = 8,
		                              .sample_rate = 48000, .src_mac = SRC };
		reac_slave_fsm_init(&s, &cfg);
		CHK(s.ch_base == 0x00);
		struct reac_ctrl_parsed p;
		p = ha_rec(0x00, REAC_HEADAMP_SENS, 0x10); CHK(reac_slave_headamp_rx(&s, &p) == 0);
		p = ha_rec(0x07, REAC_HEADAMP_SENS, 0x10); CHK(reac_slave_headamp_rx(&s, &p) == 7);
		p = ha_rec(0x08, REAC_HEADAMP_SENS, 0x10); CHK(reac_slave_headamp_rx(&s, &p) == -1);
	}

	/* (e) STAGE-LEVEL: a received SENS record precomputes the channel's gain, and
	 * the RT apply scales exactly that channel's samples by it — other channels stay
	 * unity. Also verifies PAD is folded in (pad on -> 20 dB less than pad off). */
	{
		struct reac_slave s;
		slave16(&s);

		/* SENS 0x10 on wire ch 0x22 = our input 2, pad off. */
		struct reac_ctrl_parsed p = ha_rec(0x22, REAC_HEADAMP_SENS, 0x10);
		CHK(reac_slave_headamp_rx(&s, &p) == 2);
		float g2 = reac_slave_headamp_gain(0x10, 0);
		CHK(approx(atomic_load_explicit(&s.ha_gain[2], memory_order_relaxed), g2));

		/* Turn the pad ON for the same input -> gain drops 20 dB (0.1x). */
		p = ha_rec(0x22, REAC_HEADAMP_PAD, 1);
		CHK(reac_slave_headamp_rx(&s, &p) == 2);
		float g2pad = reac_slave_headamp_gain(0x10, 1);
		CHK(approx(atomic_load_explicit(&s.ha_gain[2], memory_order_relaxed), g2pad));
		CHK(approx(g2pad, g2 * 0.1f));

		/* Pad it back off, and set a different SENS on input 5 (wire ch 0x25). */
		p = ha_rec(0x22, REAC_HEADAMP_PAD, 0);   reac_slave_headamp_rx(&s, &p);
		p = ha_rec(0x25, REAC_HEADAMP_SENS, 0x04); CHK(reac_slave_headamp_rx(&s, &p) == 5);
		float g5 = reac_slave_headamp_gain(0x04, 0);

		/* Phantom is a voltage, not a gain: it updates state but must NOT touch the
		 * channel's gain. */
		float before = atomic_load_explicit(&s.ha_gain[7], memory_order_relaxed);
		p = ha_rec(0x27, REAC_HEADAMP_PHANTOM, 1); CHK(reac_slave_headamp_rx(&s, &p) == 7);
		CHK(s.ha_phantom[7] == 1);
		CHK(atomic_load_explicit(&s.ha_gain[7], memory_order_relaxed) == before);
		CHK(before == 1.0f);

		/* Apply the whole 16-wide table to a unit-ramp planar buffer. */
		float buf[16][REAC_SAMPLES_PER_PKT];
		float *planar[16];
		for (int c = 0; c < 16; c++) {
			planar[c] = buf[c];
			for (int i = 0; i < REAC_SAMPLES_PER_PKT; i++)
				buf[c][i] = 1.0f;
		}
		reac_slave_apply_input_gain(planar, 16, REAC_SAMPLES_PER_PKT, s.ha_gain);
		for (int i = 0; i < REAC_SAMPLES_PER_PKT; i++) {
			CHK(approx(buf[2][i], g2));    /* input 2 scaled by its SENS gain */
			CHK(approx(buf[5][i], g5));    /* input 5 by its own              */
			CHK(buf[0][i] == 1.0f);        /* untouched inputs stay unity     */
			CHK(buf[7][i] == 1.0f);        /* phantom-only channel: no gain    */
			CHK(buf[15][i] == 1.0f);
		}
	}

	printf("test_reac_vboxgain: OK\n");
	return 0;
}
