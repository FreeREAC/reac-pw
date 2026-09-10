// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* THE PREAMP DOOR ON A SEGMENT A STAGEBOX MASTERS — the bytes, not the acceptance.
 *
 * Operator ruling, 2026-09-10, verbatim: "the clock owner has nothing to do with normal
 * audio operations, only enrolment"; "we sync it and we should be able to set the pre-amp
 * params as usual, no changes"; "there is no change in the protocol once we exchange
 * frames, it is exactly the same". Until then reac_slave had only the RECEIVE half of the
 * head-amp — a virtual box being told what its preamps do — and a console's 48V switch on
 * a joined box-master segment reached a table nothing ever emitted from.
 *
 * WHAT THIS ASSERTS, AND WHY THAT IS THE RIGHT SEAM. The one thing that can be proven off
 * the rig is that the SET this engine puts on the wire is byte-for-byte the SET the master
 * role puts on the wire for the same cell. The goldens below are not built from our own
 * second call: they are the four distinct `cdea 04 03` blocks reac-pw actually broadcast
 * at the S-1608 on 2026-09-09 (ha-write-s1608.pcap, 1492 B, dst ff:ff:ff:ff:ff:ff) — ch
 * 0x20 (the S-1608's head-amp base, its input 1), params 00/01/02 = phantom/pad/SENS, the
 * DT1 record checksum, f7, and the block checksum 0x02.
 *
 * WHAT IT CANNOT ASSERT: whether the box ACTS on a SET from a peer it is mastering. A
 * byte-identical record reached an S-0808 on M on 2026-09-09 and its preamp did not move.
 * That is a fact about the box, it is settled only by watching a 48V lamp, and no test in
 * this repo may claim it. This one claims the record leaves. */
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_headamp_tx.h>   /* REAC_HEADAMP_SWEEP_STRIDE */
#include <reac/reac_encode.h>

#include "reac_ring.h"
#include "reac_slave.h"

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* The four blocks the rig sent, as frame[16:50] hex. */
static const struct { uint8_t ch, param, value; const char *want; } RIG[] = {
	{ 0x20, 0, 0,    "cdea04030013000200fe0ef0410a0000121201012000005ef7000000000000000002" },
	{ 0x20, 0, 1,    "cdea04030013000200fe0ef0410a0000121201012000015df7000000000000000002" },
	{ 0x20, 1, 0,    "cdea04030013000200fe0ef0410a0000121201012001005df7000000000000000002" },
	{ 0x20, 2, 0x34, "cdea04030013000200fe0ef0410a00001212010120023428f7000000000000000002" },
};

static void hex34(char *out, const uint8_t *frame)
{
	for (int i = 0; i < 34; i++)
		sprintf(out + i * 2, "%02x", frame[16 + i]);
}

int main(void)
{
	static const uint8_t SRC[6] = { 0x00, 0x40, 0xab, 0x9f, 0x9e, 0xbe };
	struct reac_slave_cfg cfg = { .ifname = "none", .box_channels = 8,
	                              .sample_rate = 48000, .src_mac = SRC,
	                              .box_master = 1, .tag = "" };
	struct reac_slave s;
	reac_slave_fsm_init(&s, &cfg);

	/* The carrier: the 40-slot mixer frame this engine puts on a box-master wire
	 * (reac_slave.c's bm_downstream), with a distinct constant per channel so a stamp
	 * that reached into the audio is visible. */
	float pcm[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
	float *planar[REAC_MAX_CHANNELS];
	for (int c = 0; c < REAC_MAX_CHANNELS; c++) {
		planar[c] = pcm[c];
		for (int t = 0; t < REAC_SAMPLES_PER_PKT; t++)
			pcm[c][t] = (float)(c + 1) / 64.0f;
	}
	uint8_t frame[2048], pristine[2048];
	int flen = reac_downstream_build(frame, (float *const *)planar, REAC_MAX_CHANNELS,
	                                 REAC_SAMPLES_PER_PKT, 0x1234, SRC);
	CHK(flen == 1492);
	memcpy(pristine, frame, (size_t)flen);

	/* SILENT UNTIL SOMEBODY ASKS. An empty table emits nothing, so a segment where no
	 * operator has touched a preamp puts the same bytes on the wire as before this door
	 * existed. Asserted first, because it is what makes every count below mean something. */
	CHK(reac_slave_headamp_stamp(&s, frame, 0, 1) == 0);
	CHK(memcmp(frame, pristine, (size_t)flen) == 0);
	CHK(atomic_load(&s.ha_tx_records) == 0);

	/* THE GUARDS, before the emission — a door that cannot refuse is not a door.
	 * A queued cell must NOT go out over another control block, and must NOT go out
	 * before the pairing is established. */
	CHK(reac_slave_headamp_set(&s, RIG[1].ch, RIG[1].param, RIG[1].value) == 1);
	CHK(reac_slave_headamp_drain(&s) == 1);
	CHK(reac_slave_headamp_stamp(&s, frame, 1 /* the slot carries a grant */, 1) == 0);
	CHK(reac_slave_headamp_stamp(&s, frame, 0, 0 /* not established */) == 0);
	CHK(memcmp(frame, pristine, (size_t)flen) == 0);
	CHK(atomic_load(&s.ha_tx_records) == 0);

	/* THE JOB: phantom ON for the box's input 1 leaves as the rig's own bytes. */
	CHK(reac_slave_headamp_stamp(&s, frame, 0, 1) == 1);
	char got[128];
	hex34(got, frame);
	CHK(strcmp(got, RIG[1].want) == 0);
	CHK(reac_ctrl_headamp_record_verify(frame) == 0);
	CHK(atomic_load(&s.ha_tx_records) == 1);
	/* AND NOTHING ELSE MOVED: the addresses, the counter and all 40 channels of audio
	 * plus the end marker are the bytes reac_downstream_build wrote. */
	CHK(memcmp(frame, pristine, 16) == 0);
	CHK(memcmp(frame + 50, pristine + 50, (size_t)flen - 50) == 0);

	/* ONE RECORD PER SLOT, AND THE EDGE IS SENT MORE THAN ONCE. The protocol has no
	 * readback, so a single frame carrying a 48V command means one lost frame is one
	 * lost setting for ever; this segment has no establishment scene replay to repair
	 * that (no announced base to sweep from), so the edge itself repeats — three sends
	 * spaced by the protocol's own record stride. Asserted by walking slots: the
	 * repeats must be SPACED (a slot in between carries nothing) and must carry the
	 * SAME cell, and after the third the wire goes quiet for good. */
	{
		int sends = 1;                       /* the one already stamped above */
		int gaps = 0;
		for (int slot = 0; slot < REAC_HEADAMP_SWEEP_STRIDE * 8; slot++) {
			memcpy(frame, pristine, (size_t)flen);
			if (reac_slave_headamp_stamp(&s, frame, 0, 1)) {
				sends++;
				hex34(got, frame);
				CHK(strcmp(got, RIG[1].want) == 0);   /* the same cell, again */
			} else {
				gaps++;
				CHK(memcmp(frame, pristine, (size_t)flen) == 0);
			}
		}
		CHK(sends == REAC_SLAVE_HEADAMP_EDGE_SENDS);
		/* SPACED, not bunched: two repeats cost at least a stride of silence each. */
		CHK(gaps >= REAC_HEADAMP_SWEEP_STRIDE);
	}
	/* AND THEN IT IS QUIET FOR GOOD — the repeat is loss tolerance, not a re-assert,
	 * and a segment nobody is touching must not keep writing 48V at a box. */
	for (int slot = 0; slot < 500; slot++) {
		memcpy(frame, pristine, (size_t)flen);
		CHK(reac_slave_headamp_stamp(&s, frame, 0, 1) == 0);
		CHK(memcmp(frame, pristine, (size_t)flen) == 0);
	}

	/* EVERY CELL THE RIG SENT, each one its own bytes. Distinct goldens are what makes
	 * a stamp that ignored its arguments fail here instead of passing four times. */
	for (size_t k = 0; k < sizeof RIG / sizeof RIG[0]; k++) {
		memcpy(frame, pristine, (size_t)flen);
		CHK(reac_slave_headamp_set(&s, RIG[k].ch, RIG[k].param, RIG[k].value) == 1);
		CHK(reac_slave_headamp_drain(&s) == 1);
		CHK(reac_slave_headamp_stamp(&s, frame, 0, 1) == 1);
		hex34(got, frame);
		CHK(strcmp(got, RIG[k].want) == 0);
	}
	/* The first phantom edge plus its two repeats, then one stamp per rig cell (each
	 * SET re-arms the repeater, and only its first send is taken here). */
	CHK(atomic_load(&s.ha_tx_records) ==
	    (uint64_t)(REAC_SLAVE_HEADAMP_EDGE_SENDS + (int)(sizeof RIG / sizeof RIG[0])));

	/* THE PROBE CAN SEE A DIFFERENCE: a cell the rig did not send must not produce the
	 * rig's bytes. Without this, a stamp hard-coded to one record would pass everything
	 * above (the four goldens differ, but a broken loop that always emitted RIG[3] would
	 * only fail three of them — this fails a stamp that ignores its channel). */
	memcpy(frame, pristine, (size_t)flen);
	CHK(reac_slave_headamp_set(&s, 0x21, 2, 0x34) == 1);
	CHK(reac_slave_headamp_drain(&s) == 1);
	CHK(reac_slave_headamp_stamp(&s, frame, 0, 1) == 1);
	hex34(got, frame);
	for (size_t k = 0; k < sizeof RIG / sizeof RIG[0]; k++)
		CHK(strcmp(got, RIG[k].want) != 0);
	CHK(reac_ctrl_headamp_record_verify(frame) == 0);

	/* A BAD CELL IS DROPPED AT THE TABLE, not emitted as something else: phantom is
	 * boolean and there is no param 9. reac_headamp_tx_set rejects both, so the drain
	 * counts them and the wire stays as it was. */
	memcpy(frame, pristine, (size_t)flen);
	CHK(reac_slave_headamp_set(&s, 0x20, 0, 0x02) == 1);
	CHK(reac_slave_headamp_set(&s, 0x20, 9, 0x00) == 1);
	CHK(reac_slave_headamp_drain(&s) == 2);
	CHK(reac_slave_headamp_stamp(&s, frame, 0, 1) == 0);
	CHK(memcmp(frame, pristine, (size_t)flen) == 0);

	/* THE RING IS BOUNDED AND SAYS SO. An absolute value is not a delta, so a dropped
	 * command is superseded rather than lost as an offset — but the drop is counted,
	 * because a silently full queue is a control that stops working without saying so. */
	for (int i = 0; i < REAC_SLAVE_HEADAMP_CMD_RING; i++)
		CHK(reac_slave_headamp_set(&s, 0x20, 2, (uint8_t)(i % 0x38)) == 1);
	CHK(reac_slave_headamp_set(&s, 0x20, 2, 0x01) == 0);
	CHK(atomic_load(&s.ha_tx_drops) == 1);
	CHK(reac_slave_headamp_drain(&s) == REAC_SLAVE_HEADAMP_CMD_RING);

	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("OK: a slave-role segment joined to a box master emits the head-amp SET the rig "
	       "itself sent — byte-identical [16:50], record checksum valid, audio and counter "
	       "untouched, refused over a control slot and before ESTABLISHED\n");
	return 0;
}
