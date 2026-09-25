// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* GOLDEN REFERENCE — captured off a real Roland M-300 (00:40:ab:c9:d8:5b)
 * driving a real S-1608, reac-captures/m300-s1608-*.pcap (2026-07-10). These are
 * the bytes a genuine master puts on the wire; the generator in libreac's
 * reac_master.c must reproduce them (#130).
 *
 * Bytes are frame[16:50] = REAC type[2] + control block[32] (checksum last), the
 * same framing reac_m200_golden.inc uses.
 *
 * ONE COPY, ONE FIXTURE. test_reac_master.c and test_reac_s1608.c each carried
 * their own transcription of these two blocks under different names
 * (GOLD_CHANMAP/GOLD_CFEA_M300 and CAP_CHANMAP/CAP_CFEA). Two copies of a
 * captured block are two things that can disagree about what the wire said, and
 * the one that is not edited quietly becomes the authority for whichever test
 * still reads it. The protocol is owned by libreac and spec/reac.ksy; a golden in
 * this repo is evidence about conformance, and evidence is kept once. */

/* The chanmap's marker window (cdea 01 03 0019, sel2 = fe): the section marker
 * followed by slots 0x00..0x06. It is window 0 of the 49-window head-amp sweep,
 * and byte-identical to GOLD_CHANMAP_SWEEP[0] in reac_m200_golden.inc — the map
 * carries no MAC and no pace-dependent byte in this window, so an M-200 and an
 * M-300 emit the same bytes here. The two goldens stay separate because their
 * PROVENANCE differs; that they agree is a finding, not a reason to alias one to
 * the other. */
static const uint8_t M300_CHANMAP_FE[34] =
 { 0xcd,0xea,0x01,0x03,0x00,0x19,0x01,0xfe,0x00,0x00,0x00,0x28,0x00,0x01,0x28,0x00,0x02,0x28,0x00,0x03,0x28,0x00,0x04,0x28,0x00,0x05,0x28,0x00,0x06,0x28,0x00,0x00,0x00,0xb7 };

/* The cfea announce with the captured M-300's own MAC still embedded at
 * [11:17]. Our master emits this block with OUR MAC substituted and the checksum
 * recomputed (the identity fix: the announced identity must equal the L2 source),
 * so a generated frame matches this one everywhere except those seven bytes —
 * and matches it EXACTLY when our MAC is the M-300's.
 *
 * Its last three payload bytes are not padding: [18] is the upstream width in
 * force (0x08 idle), [19] the pace code (reac_pace_code: 0x00 48k / 0x01 96k /
 * 0x02 44.1k) and [20:22] the enrolled-box count, u2 big-endian. This capture is
 * an idle desk, so the count is 0. */
static const uint8_t M300_CFEA[34] =
 { 0xcf,0xea,0xff,0xff,0x01,0x00,0x01,0x03,0x0d,0x01,0x04,0x00,0x40,0xab,0xc9,0xd8,0x5b,0x28,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xd4 };

/* Offsets into the 34-byte [type|block] cfea template. The MAC index is the
 * authoritative [11:17] (an early task note said "byte[14:17]"). */
#define M300_CFEA_MAC_IDX   (REAC_TYPE_WORD_BYTES + REAC_ANNOUNCE_MAC_OFF)
#define M300_CFEA_CKSUM_IDX (REAC_CTRL_CKSUM_OFF - REAC_TYPED_BLOCK_OFF)
