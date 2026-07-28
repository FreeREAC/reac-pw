// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_slots — the TWO REAC slot spaces, deliberately kept apart.
 *
 * A box's inputs are addressed in two DIFFERENT spaces, and conflating them has
 * produced a real bug in each direction. This header names both, once, with the
 * capture evidence for each bound; nothing in this repo should spell 40, 48 or
 * 0x2f as a bare literal again.
 *
 * ---- AUDIO FABRIC — 40 slots (0..39) -------------------------------------
 * What a downstream frame actually CARRIES: 40 channels x 12 samples
 * (libreac REAC_MAX_CHANNELS, REAC_FRAME_BYTES). The master ADVERTISES exactly
 * this width in the cfea capability block, byte [17] = 0x28 = 40 — see the
 * captured M-300 cfea golden in tests/test_reac_s1608.c (`… 28 08 …`), and the
 * ENROLL group map (cdea 01 03 000d) spans exactly those 40 as 5 groups x 8
 * (docs/PLACEMENT-EVIDENCE.md, 42 grant sweeps across 82 captures).
 *
 * This is the space reac_boxreg allocates in: a box's audio must LAND inside the
 * 40 slots the wire carries, so a placement ending past slot 39 is unrepresentable
 * — the channels simply would not exist in the frame.
 *
 * ---- HEAD-AMP / CHANMAP space — 48 slots (0x00..0x2f) ---------------------
 * The CH of a head-amp record (op 04 03, TAG 01 01) is a WIRE channel =
 * model_base + (box_input - 1), and the chanmap ring (cdea 01 03 0019) advertises
 * those 48 positions followed by the 0xfe section marker — 49 windows, measured
 * live off an M-200 driving an S-1608 (#130).
 *
 * PROOF THE TWO ARE NOT ONE SPACE: every desk in the corpus places an S-1608 at
 * base 0x20 (12 sweeps, M-200i + M-300 + M-5000, docs/PLACEMENT-EVIDENCE.md), and
 * the box is 16 wide — so its group-A run reaches 0x20 + 16 - 1 = 0x2f = 47, past
 * 40. A group-A CH therefore CANNOT be an audio-fabric slot index.
 *
 * ---- Why both directions are load-bearing ---------------------------------
 * Bounding the HEAD-AMP space by 40 silently rejects an S-1608's inputs 9..16
 * (CH 32..47): they can never be given phantom/pad/sens. That bug was real and was
 * fixed twice already (reac_ctrl.h's REAC_HEADAMP_MAX_CH, reac_headamp_tx) — it is
 * the "48V never lit" class from the July 2026 campaign.
 *
 * Bounding an AUDIO allocation by 48 is the same mistake mirrored: a multi-box
 * placement would be accepted running to slot 47 and would silently overflow the
 * 40-slot frame, with channels going missing only once real boxes are on the wire.
 *
 * So: 0x2f is a real, hard ceiling — the HEAD-AMP one. Anything deciding where a
 * box's AUDIO lives takes REAC_AUDIO_FABRIC_*; anything addressing a head-amp or
 * chanmap CH takes REAC_HEADAMP_*. Never one for the other (#69, #129, #210).
 */
#ifndef REAC_SLOTS_H
#define REAC_SLOTS_H

/* AUDIO fabric: the slots a downstream frame carries and the master advertises
 * (cfea [17] = 0x28). Allocation of a box's audio lives here. */
#define REAC_AUDIO_FABRIC_SLOTS   40
#define REAC_AUDIO_FABRIC_CEILING (REAC_AUDIO_FABRIC_SLOTS - 1)   /* 39 = 0x27 */
/* MUTATION-CHECKED: widening this to 48 makes tests/test_reac_grant.c section 5b
 * accept a 16-wide box at audio slot 32 (ending at 47) — the test fails. */

/* HEAD-AMP / chanmap wire-channel space: 0x00..0x2f. An S-1608 based at 0x20
 * occupies 0x20..0x2f, which is why this ceiling is 0x2f and not 39. */
#define REAC_HEADAMP_SLOTS        48
#define REAC_HEADAMP_CEILING      (REAC_HEADAMP_SLOTS - 1)        /* 0x2f = 47 */

/* The chanmap RING: the 48 head-amp positions plus the 0xfe section marker at the
 * wrap, so a full sweep is exactly 49 windows (#130). */
#define REAC_HEADAMP_RING         (REAC_HEADAMP_SLOTS + 1)        /* 49 */

#endif /* REAC_SLOTS_H */
