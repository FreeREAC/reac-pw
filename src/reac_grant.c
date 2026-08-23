// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_grant.h"
#include "reac_ctrl.h"        /* reac_ctrl_stamp_headamp, enum reac_headamp_param */
#include "reac_headamp_tx.h"  /* struct reac_headamp_tx */

#include <reac/reac.h>        /* REAC_FRAME_BYTES */
#include <reac/reac_ports.h>  /* the announced-base law + REAC_HEADAMP_BASE_* */
#include <string.h>

/* ---- The allocator ------------------------------------------------------- *
 *
 * EVIDENCE — the WHOLE capture corpus, not three goldens. docs/PLACEMENT-EVIDENCE.md
 * (#210) streams all 82 captures in reac-captures (~19 GB); 42 of them carry a grant
 * sweep, and those 42 collapse to exactly three observed placements:
 *
 *   box      inputs  observed base  group-A slots  sweeps  desks that agree
 *   S-0808     8        0x00          0x00..0x07     11    M-200i, M-300, M-5000
 *   S-1608    16        0x20          0x20..0x2f     21    M-200i, M-300, M-5000
 *   S-4000S   32        0x00          0x00..0x1f      5    M-200i, M-5000
 *
 * Every sweep is contiguous; no two sweeps of the same box ever disagree.
 *
 * WHAT THE CORPUS SETTLES. The base is a deterministic function of WHAT THE BOX
 * DECLARES AT COLD-CONNECT, and of nothing else we can name. Dead, each by capture:
 *   - lowest-fit (predicts 0x00 for a 16-wide box) and top-aligned-to-0x2f (predicts
 *     0x28 for an S-0808, 0x10 for an S-4000S) — both contradicted outright;
 *   - f(desk): three desk models grant the SAME box the SAME base, and ONE M-200i
 *     grants 0x00 and 0x20 to two boxes in one session (ctl2.pcap);
 *   - f(enrolment order) / next-free: 22 consecutive re-joins in one capture, zero
 *     drift (s0808-reboot-enrollfix);
 *   - f(box MAC / unit identity): reac-pw's slave on a DIFFERENT MAC that merely
 *     DECLARES the S-1608's blocks is granted 0x20 by a real M-200 (9 sweeps);
 *   - f(ENROLL group map 0103 000d): the map is a pure width function, front-packed
 *     from group 0 — and 17 of 18 real-S-1608 captures never receive one at all;
 *   - f(CHANMAP 0103 0019): byte-identical full 49-position ring for every box.
 *
 * WHAT IT STILL DOES NOT SETTLE. Three carriers stay perfectly collinear across all
 * 42 rows, because we own only three declaration variants: the declared WIDTH (this
 * table), the config-announce selector byte (0x82 vs 0x84), and config-announce byte
 * [9] (0x02 vs 0x00, for which base == byte[9] * 0x10 holds on every row). Naming any
 * one of them "the law" would be picking one of three indistinguishable hypotheses.
 * PLACEMENT-EVIDENCE.md ends with the 5-run rig experiment that separates them — a
 * one-byte patch to our own slave declaration, no new hardware.
 *
 * SO: we keep pinning the OBSERVED base per width. Reasons, in order: (a) it predicts
 * all 42 rows and is the only placement each real box is known to have accepted;
 * (b) the S-1608's 0x20 origin is already baked into the rest of the stack as that
 * model's head-amp CH base (reac_ctrl.h's head-amp block comment, openmixer's
 * channel mapping), so choosing differently here would silently desync them;
 * (c) a wrong-but-self-consistent allocation is exactly the failure we are fixing —
 * being consistent with the REST OF THE WORLD is the whole point.
 *
 * NOTE ON THE CEILING — RESOLVED IN CODE (#69). The 0x2f ceiling this allocator
 * enforces is the HEAD-AMP / chanmap channel space (48 slots), NOT the audio fabric:
 * cfea advertises 40 audio slots ([17] = 0x28) and the ENROLL map spans exactly those
 * 40 (5 groups x 8). The S-1608 runs to 0x2f = 47, past 40 — so group-A CH is not an
 * audio-fabric index. The constant used to be spelled REAC_GRANT_FABRIC_CEILING,
 * which named the audio fabric while measuring the head-amp space; both spaces are
 * now defined once, honestly, in reac_slots.h and this allocator takes the head-amp
 * one. Where a box's AUDIO lands stays reac_boxreg's decision over
 * REAC_AUDIO_FABRIC_SLOTS. Multi-box allocation (#129) must keep them apart.
 *
 * THE BASE IS NOT OURS TO CHOOSE. It is the box's own chassis strap, announced
 * in its config announce at block[7] and multiplied by 0x10 (libreac
 * reac_ports.h). This function used to derive it from the box's INPUT WIDTH via
 * libreac's reac_headamp_base(), which agreed with the wire on all three chassis
 * we own only because width and strap are collinear on them — a 16-input chassis
 * always straps 2. A master cannot move where a head-amp write lands by granting
 * differently; nothing in the box consumes a granted base.
 *
 * So what is left here is not allocation, it is ADMISSION: the box states its
 * base, and we check the slots it claims fit the head-amp space. They may not —
 * width 32 at base 0x20 runs to 0x3f, past the 0x2f ceiling — and a box that
 * does not fit is REFUSED, never quietly moved to a base it did not ask for.
 * Multi-box coexistence (#129) is a question of whether two announced bases
 * overlap, not of where to put them. */

int reac_grant_alloc_fits(int base, int width)
{
	if (width <= 0 || width > REAC_GRANT_MAX_WIDTH)
		return 0;
	if (base < 0)
		return 0;
	/* The ceiling test, stated as the doc states it: the LAST slot the box would
	 * own is base+width-1, and it must not pass 0x2f. HEAD-AMP space — see the
	 * ceiling note above and reac_slots.h: 0x20+16 = 0x2f is legal here precisely
	 * because it is NOT an audio-fabric index. */
	return (base + width - 1) <= REAC_HEADAMP_CEILING;
}

int reac_grant_allocate(struct reac_grant_alloc *out, int base, int in_ch)
{
	if (!out || in_ch <= 0 || in_ch > REAC_GRANT_MAX_WIDTH)
		return -1;

	/* The box announced this base. Admit it if the slots it claims fit the
	 * head-amp space, and refuse if they do not — there is no second choice to
	 * fall back to. Moving a box to a base it did not announce writes its
	 * head-amp records to slots its preamps never read. */
	if (!reac_grant_alloc_fits(base, in_ch))
		return -1;
	out->base  = (uint8_t)base;
	out->width = (uint8_t)in_ch;
	return 0;
}

/* ---- The group-A values -------------------------------------------------- *
 *
 * The scene value for a cell — the operator's if set, else the enrolling default
 * (phantom/pad safe-off, SENS deliberately non-zero so the channel enrols; the
 * WHY lives with the defaults, reac_headamp_default). The merge lives with the
 * table (reac_headamp_tx_effective) because the post-establish scene REPLAY must
 * arm byte-identical values to this grant sweep — both draw from the one
 * function.
 *
 * OPEN (pending live S-1608 confirmation): whether SENS-alone with phantom=0 is
 * enough to enrol, and the minimal enrolling value. Until proven, arm a known-good
 * real value rather than probe for the floor.
 */
uint8_t reac_grant_headamp_value(const struct reac_headamp_tx *tx,
                                 uint8_t ch, uint8_t param)
{
	return reac_headamp_tx_effective(tx, ch, param);
}

/* ---- The fixed scaffolding ----------------------------------------------- *
 * Both goldens (m200 x S-0808 and m200 x S-1608) open the burst with the same two
 * cdea 04 03 0014 head frames and carry the same 6 group-B records, byte-identical
 * across box widths. Only group A scales. Deduped by frame counter from
 * matrix-m200-s{0808,1608}-2026-07-11.pcap (the mirror tap duplicates every frame;
 * see the ordering note below). Each row already sums to 0 over [18:50], so
 * reac_master_stamp's checksum re-stamp is a no-op and the on-wire bytes equal a
 * real M-200's. */

/* The sweep's SHAPE — which records, in which order, with which markers — is
 * protocol and lives in libreac (reac_ctrl_build_grant_sweep). What stays here is
 * the POLICY this file has always been the seam for: which slots a box is given,
 * and what value each cell should carry. We gather the values and hand them over.
 */
int reac_grant_build_sweep(uint8_t sweep[][34], int max,
                           const struct reac_grant_alloc *alloc,
                           const struct reac_headamp_tx *tx)
{
	if (!sweep || !alloc)
		return -1;
	int w = alloc->width;
	if (!reac_grant_alloc_fits(alloc->base, w))
		return -1;

	/* One cell per (allocated channel, parameter), in the order the sweep wants
	 * them. reac_grant_headamp_value is the policy: the operator's setting where
	 * there is one, the enrolling default where there is not. */
	uint8_t values[REAC_GRANT_MAX_WIDTH * REAC_HEADAMP_NPARAMS];
	if (w <= 0 || w > REAC_GRANT_MAX_WIDTH)
		return -1;
	for (int c = 0; c < w; c++) {
		uint8_t ch = (uint8_t)(alloc->base + c);
		for (uint8_t p = 0; p < REAC_HEADAMP_NPARAMS; p++)
			values[c * REAC_HEADAMP_NPARAMS + p] =
				reac_grant_headamp_value(tx, ch, p);
	}
	return reac_ctrl_build_grant_sweep(sweep, max, alloc->base, w, values);
}
