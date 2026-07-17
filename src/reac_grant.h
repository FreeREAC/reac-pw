// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_grant — the MASTER's per-channel ENROLLMENT SWEEP (the cdea 04 03 grant).
 *
 * A real master does not merely echo a box's cold-connect back: after the box
 * joins, it runs a structured per-channel sweep that ENROLLS the box's inputs into
 * the fabric slots the master ALLOCATED to them (reac-firmware-re/GRANT-SWEEP.md).
 * Two interleaved groups ride that sweep:
 *
 *   Group A — record marker 12 12, TAG 01 01: one record per allocated channel per
 *       param. SOLVED 2026-07-17: this TAG *is* the head-amp TAG, and its three
 *       "sub-phases" 00/01/02 are exactly phantom/pad/sens. Group A is the master
 *       PUSHING each allocated channel's initial head-amp state as part of
 *       enrollment — not an opaque negotiation. It is therefore GENERATED here
 *       from our own allocation + our own head-amp state, never replayed.
 *   Group B — record marker 12 11, TAG 05 00: a fixed 6-record constant, byte-
 *       identical across 8/16/32-input boxes (GRANT-SWEEP.md, re-verified against
 *       the m200-s0808 and m200-s1608 goldens 2026-07-17). Replayed verbatim.
 *
 * WHY THIS EXISTS (the bug it fixes): reac_master.c used to replay a 32-frame
 * burst transcribed from an M-200 granting an S-0808 (8 inputs, base 0x00) at ANY
 * box. Handed to a 16-input S-1608 (base 0x20) the box LINKS — but every later
 * head-amp record addressing CH 0x20 names a slot our own grant never claimed, so
 * the box ignores it (live 2026-07-17: 28 byte-perfect head-amp records in 40 s,
 * 48V never lit). Link established != channels enrolled — the same root cause as
 * #130's mute box.
 *
 * PURE: no socket, no frame buffer, no FSM. Emits 34-byte [type|block] templates
 * ([16:50]) that reac_master_stamp applies exactly like the old tables.
 */
#ifndef REAC_GRANT_H
#define REAC_GRANT_H

#include <stdint.h>

struct reac_headamp_tx;   /* reac_headamp_tx.h — the per-channel head-amp state */

/* ---- The fabric ---------------------------------------------------------- *
 * The REAC fabric addresses channels 0x00..0x2f. 0x2f is a HARD ceiling: it is
 * where reac_master.c's chanmap ring wraps to the 0xfe section marker, and a grant
 * that ran past it would claim slots the chanmap can never advertise. */
#define REAC_GRANT_FABRIC_CEILING 0x2f            /* highest addressable slot   */
#define REAC_GRANT_FABRIC_SLOTS   (REAC_GRANT_FABRIC_CEILING + 1)   /* 48 */

/* The widest box we can enroll (S-4000S = 32 inputs). */
#define REAC_GRANT_MAX_WIDTH 32

/* The sweep's frame count for a width-w box: 2 fixed head frames + 6 group-B
 * records + w*3 group-A records. w=8 -> 32, w=16 -> 56, w=32 -> 104 — matching the
 * three real M-200/M-5000 goldens frame-for-frame. */
#define REAC_GRANT_SWEEP_LEN(w) (8 + (w) * 3)
#define REAC_GRANT_SWEEP_MAX    REAC_GRANT_SWEEP_LEN(REAC_GRANT_MAX_WIDTH)   /* 104 */

/* A slot allocation: the box's `width` inputs occupy fabric slots
 * [base, base+width). Both fields are the MASTER's decision — this is a routing
 * choice, not a per-model template (GRANT-SWEEP.md). */
struct reac_grant_alloc {
	uint8_t base;
	uint8_t width;
};

/* Does [base, base+width) fit inside the fabric? Returns 1 when it does, else 0.
 * The load-bearing case: width 32 at base 0x20 would run to 0x3f, PAST the 0x2f
 * ceiling — which is exactly why a real desk is forced to base a 32-input box at
 * 0x00 (GRANT-SWEEP.md, verified on an M-5000 x two S-4000S units). */
int reac_grant_alloc_fits(int base, int width);

/* Allocate fabric slots for a box of `in_ch` inputs. Fills *out and returns 0, or
 * returns -1 (leaving *out untouched) for a width we cannot place.
 *
 * POLICY (see reac_grant.c for the evidence + its limits): each known box width
 * has an OBSERVED base — the placement a real desk was captured using for that box
 * — which we reproduce because it is the only placement proven to interoperate,
 * and because the rest of the stack already encodes it as the box's head-amp CH
 * origin. The observed base is then validated against the fabric ceiling, and any
 * width without an observed base (or whose observed base does not fit) falls back
 * to the lowest base that does. */
int reac_grant_allocate(struct reac_grant_alloc *out, int in_ch);

/* Build the grant sweep for `alloc` into `sweep` (capacity `max` rows of 34 bytes).
 * Returns the number of rows written (= REAC_GRANT_SWEEP_LEN(alloc->width)), or -1
 * on a bad alloc / insufficient capacity.
 *
 * `tx` supplies the per-channel head-amp state group A pushes: for each allocated
 * wire channel and each param, the value is taken from `tx` when the operator/config
 * has SET that cell, else the documented safe default (see reac_grant.c). `tx` may
 * be NULL -> every cell takes the default.
 *
 * Every group-A record is produced by the PROVEN builder (reac_ctrl_stamp_headamp),
 * so the bytes and both nested checksums are the ones already verified byte-exact
 * against a real M-200 (tests/test_reac_headamp.c) — no record bytes are hand-rolled
 * here. */
int reac_grant_build_sweep(uint8_t sweep[][34], int max,
                           const struct reac_grant_alloc *alloc,
                           const struct reac_headamp_tx *tx);

/* The head-amp value the sweep pushes for (ch, param): `tx`'s cell when set, else
 * the safe default. Exposed so a caller (and the tests) can assert the sourcing
 * rule without re-deriving it. `tx` may be NULL. */
uint8_t reac_grant_headamp_value(const struct reac_headamp_tx *tx,
                                 uint8_t ch, uint8_t param);

#endif /* REAC_GRANT_H */
