// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// GENERATED FILE - DO NOT EDIT.
//
// Source:    spec/protocol-facts.yaml   (in FreeREAC/reac-protocol)
// Generator: spec/gen-facts.py
//
// Edit the schema and regenerate. A hand-edit here is erased by the next run
// and, worse, is invisible to the cross-check that keeps reac.ksy and libreac
// agreeing - which is the whole reason this file is generated.

#ifndef REAC_FACTS_H
#define REAC_FACTS_H

/* ---- Frame geometry ------------------------------------------------------------
 * A REAC frame is a fixed L2 header, a control block, an audio region of
 * whole 36-byte channels, and a two-byte end marker. Every length in the
 * protocol is 52 + n*36 for some channel count n, and that one law is what
 * lets a parser recover the width from the size — REAC carries no width
 * field in an audio frame.
 *
 * The 52 is 50 bytes of header plus the 2-byte end marker. A length that is
 * not 52 + n*36 is not a REAC frame. The `+2` a mirrored capture path leaves
 * on a frame belongs to the CAPTURE, not to the protocol — 0 such frames in
 * 592,762 captured off a plain NIC (census, 2026-09-21) — so it is not a
 * protocol fact and is not stated here; stripping it is the capture reader's
 * job, in ingest, before any byte reaches a parser.
 */
/* REAC's registered non-IP EtherType. [EVIDENCED (corpus) — every frame in 72
 * captures.]
 */
#define REAC_ETHERTYPE                  0x8819

/* 14 eth + 2 counter + 2 type word + 32 control block. [EVIDENCED (corpus).] */
#define REAC_L2_HEADER_LEN              50

/* Where the audio region starts. Same number as the header length, and it is
 * the same fact. [EVIDENCED (corpus) — a golden offset scan decodes real desk
 * program as noise at every other offset.]
 */
#define REAC_AUDIO_OFFSET               50

/* u16 LITTLE-endian free-running sequence counter, against the big-endian
 * everything else. [EVIDENCED (corpus).]
 */
#define REAC_HDR_COUNTER_OFF            14

/* The non-audio bytes of any frame — 50 header + 2 end marker. The 52 in `52
 * + n*36`. [EVIDENCED (corpus).]
 */
#define REAC_FRAME_OVERHEAD             52

/* The master sets the pace and the slaves follow it unconditionally. There is
 * no
 * per-rate cadence negotiation, no slave-side rate election, and no case in
 * which a box
 * declines a pace. So a frame rate is read off the master's clock, never
 * inferred from
 * a box's model or its declaration.
 * Consequence for our own docs: statements of the form "the upstream cadence
 * at rate R
 * is X" describe what the master chose, not a property the box asserts.
 *   [RULED (operator, 2026-08-23). Consistent with the corpus, which shows
 *   one pace per
 * segment and no negotiation exchange.
 * ]
 */
#define REAC_PACE_IS_THE_MASTERS        1

/* A clock rate is NOT a property of a mixer model. Any legal pace may run on
 * any desk,
 * and the two facts must never be derived from one another.
 * This is stated because our corpus makes them look coupled: each desk we own
 * has been
 * run at a single rate, so model and rate are perfectly correlated in the
 * captures. That
 * correlation is an accident of how the rig was used, not a fact about the
 * protocol, and
 * anything reading a per-desk field as a rate class must justify it on its
 * own evidence.
 *   [RULED (operator, 2026-08-23) — LAW. Not an inference from the corpus,
 *   and it overrides
 * any correlation the corpus appears to show.
 * ]
 */
#define REAC_RATE_IS_INDEPENDENT_OF_MODEL 1

/* The config-announce console_field byte (cfea[19]) PROPOSES the box's rate
 * CLASS; the
 * scene body's `revision` field (+0x14, SCENE_REVISION_OFF) is what a box
 * actually
 * latches. The value is the PACE CODE: 0x00 = 48 kHz, 0x01 = 96 kHz, 0x02 =
 * 44.1 kHz.
 * The same code rides four fields of one enrolment: cfea[19], the scene
 * body's
 * `revision`, the chanmap section marker's flags byte (`fe <code> 00`) and
 * the ENROLL
 * group map's console byte (ENROLL[8]). Enforcement is FIRMWARE-DEPENDENT: an
 * S-0808 (fw 1.003)
 * follows console_field alone; an S-1608 (fw 2.200) requires the scene's
 * `revision` to
 * agree with it, and otherwise holds its prior rate across a full cold
 * power-cycle.
 * This does NOT contradict RATE_IS_INDEPENDENT_OF_MODEL — it is the specific,
 * evidence-justified case that ruling anticipated ("anything reading a
 * per-desk field as a
 * rate class must justify it on its own evidence"). The rate is still the
 * master's choice
 * (PACE_IS_THE_MASTERS); console_field and `revision` are the two wire fields
 * by which the
 * master DECLARES and then RECORDS the class, and the family label (V-Mixer /
 * OHRCA) is
 * only its name. What is independent of a mixer MODEL is a bare clock number;
 * what gates
 * the class is this announce-plus-scene pair.
 * RULED (operator, 2026-09-13): the field is the pace code, not a desk
 * family. An M-200
 * at 44.1 kHz writes 0x02 on all four carriers; "V-Mixer"/"OHRCA" were only
 * the names
 * of 0x00 and 0x01 while no desk had been captured at 44.1 kHz.
 *   [RIG-VERIFIED (2026-08-27): emitting 0x00 ran the live box at 48 kHz and
 *   0x01 at 96 kHz,
 * with a warm follow on a live change; box frames stayed byte-identical
 * between the two,
 * the master byte the only differing field on that box (S-0808, fw 1.003).
 * CORRECTED (2026-08-29): that confirmation was generalised to "the box" and
 * is not
 * universal. An S-1608 (fw 2.200) held 48 kHz through a runtime assertion, a
 * daemon cold
 * boot at 96 kHz and a power-cycle into an already-running 96 kHz stream, all
 * with
 * console_field=0x01 — ten 96 kHz windows all measured WIRE PACE MISMATCH at
 * 4000 pps.
 * Making the scene's `revision` agree (see SCENE_REVISION_OFF) moved it to 96
 * kHz
 * immediately and cleanly, and pinning `revision` made the box LATCH: neither
 * a console
 * assertion nor a daemon cold boot at 48 kHz moved it back, because the value
 * never
 * changed and the box never re-read.
 * LOGIC: an M-5000 running its own box at 48 kHz must emit 0x00, so the byte
 * tracks the
 * pace chosen, not the console it came from — the M-200 already shows this
 * directly,
 * reaching 0x00 at 48 kHz and 0x02 at 44.1 kHz on the very same console.
 * WIRE-CAPTURED (2026-09-13, reac-captures m200-enrol-441k-2026-09-13 and
 * m200-enrol-s4000-441k-2026-09-13): an M-200 at 44.1 kHz (3675 fps measured)
 * enrolling an
 * S-1608 and an S-4000S emits cfea[19]=0x02 (46 announces), scene
 * `revision`=0x0002 (three
 * bodies, byte-identical), chanmap marker flags 0x02 (6 sweeps) and
 * ENROLL[8]=0x02 — the
 * same M-200 at 48 kHz writes 0x00 on every one of them, the eleven other map
 * bytes
 * identical. Both boxes paced 44.1 kHz.
 * ]
 */
#define REAC_CONSOLE_FIELD_GATES_RATE   1

/* 12 samples x 3 bytes, per channel, at EVERY sample rate. The 36 in `52 +
 * n*36`. [EVIDENCED (corpus) at 48k and 96k; RATE-INVARIANT BY CONSTRUCTION
 * (12 x 3).
 * EVIDENCED (corpus) at 44.1k since 2026-09-13: the M-200 sessions in
 * m200-enrol-441k-2026-09-13 and m200-enrol-s4000-441k-2026-09-13 measure
 * 3675 fps from the
 * pcap timestamps, with the same 52 + n*36 frame length.
 * ]
 */
#define REAC_BYTES_PER_CHANNEL          36

/* Samples per channel per frame. The RATE is the packet rate; the frame does
 * not change shape with it. [EVIDENCED (corpus).]
 */
#define REAC_SAMPLES_PER_PKT            12

/* Bytes per sample per channel — 24-bit. [EVIDENCED (corpus).] */
#define REAC_RESOLUTION                 3

/* The downstream audio fabric, 40 slots. NOT the head-amp channel space,
 * which is 48 wide; conflating the two was a real bug. [EVIDENCED (corpus).]
 */
#define REAC_MAX_CHANNELS               40

/* The fixed downstream broadcast — 52 + 40*36. [EVIDENCED (corpus).] */
#define REAC_FRAME_BYTES                1492

/* 40 ch x 12 samples x 3 B. [EVIDENCED (corpus).] */
#define REAC_AUDIO_BYTES                1440

/* First byte of the two-byte end marker. [EVIDENCED (corpus).] */
#define REAC_END_MARKER_0               0xc2

/* Second byte of the end marker. [EVIDENCED (corpus).] */
#define REAC_END_MARKER_1               0xea

/* ---- The 32-byte control block and its two checksums ---------------------------
 * Every non-FILLER frame carries a 32-byte control block at frame[18:50],
 * whose last byte is a checksum over the block. A FILLER frame (type word
 * 0x0000) carries audio there instead and is checksum-exempt.
 *
 * There are TWO checksum rules and they differ. The block sums to 0 mod 256.
 * A nested record inside it — the Roland DT1 head-amp record and its
 * relatives — sums to 0x80. The record's is stamped FIRST: a record fixed up
 * after the block is stamped invalidates the block, which is a real bug this
 * ordering exists to prevent.
 */
/* Frame offset of the control block. [EVIDENCED (corpus).] */
#define REAC_CTRL_BLOCK_OFF             18

/* One past the end — and therefore the audio offset. [EVIDENCED (corpus).] */
#define REAC_CTRL_BLOCK_END             50

/* The checksummed span. [EVIDENCED (corpus).] */
#define REAC_CTRL_BLOCK_LEN             32

/* The checksum byte, last of the block. Block-relative 31. [EVIDENCED
 * (corpus).]
 */
#define REAC_CTRL_CKSUM_OFF             49

/* The same byte, block-relative — what a caller stamping a bare 32-byte block
 * indexes. [EVIDENCED (corpus).]
 */
#define REAC_CTRL_CKSUM_OFF_IN_BLOCK    31

/* Sum(block[0..31]) mod 256 must equal this. [EVIDENCED (corpus) — every
 * control frame in the corpus.]
 */
#define REAC_CTRL_BLOCK_SUM             0x00

/* Sum(record TAG..CKSUM) mod 256 must equal this. The Roland DT1 rule, and
 * NOT the block rule. [EVIDENCED (corpus).]
 */
#define REAC_CTRL_RECORD_SUM            0x80

/* Frame offset of the type word — the start of the [16:50] window the goldens
 * store and the C tests use. [EVIDENCED (corpus).]
 */
#define REAC_TYPED_BLOCK_OFF            16

/* Type word (2) + control block (32). [EVIDENCED (corpus).] */
#define REAC_TYPED_BLOCK_LEN            34

/* ---- Type-word codes -----------------------------------------------------------
 * The two bytes at frame[16:18].
 */
/* Audio-only, no control op, checksum-exempt. [EVIDENCED (corpus).] */
#define REAC_TYPE_FILLER                0x0000

/* A control record. [EVIDENCED (corpus).] */
#define REAC_TYPE_CONTROL               0xcdea

/* The master announce. Same block shape, op always 0xffff. [EVIDENCED
 * (corpus).]
 */
#define REAC_TYPE_ANNOUNCE              0xcfea

/* ---- Control ops ---------------------------------------------------------------
 * The two bytes at block[0:2].
 */
/* A continuation chunk of the scene transfer. [EVIDENCED (image + corpus).] */
#define REAC_OP_SCENE_CHUNK             0x0100

/* The scene header, declaring the total. [EVIDENCED (image + corpus).] */
#define REAC_OP_SCENE_HEADER            0x0101

/* The final chunk. The box's commit is gated on it. [EVIDENCED (image +
 * corpus).]
 */
#define REAC_OP_SCENE_FINAL             0x0102

/* Class 1 carried whole in one frame — the fragment field is 3, both FIRST
 * and LAST. payload[0] is the only discriminator; op_len beside it is a
 * length, not a second selector. See the control_header group. [EVIDENCED
 * (image + executed trace).]
 */
#define REAC_OP_PAGE_0103               0x0103

/* Link 4 with the segment field reading FIRST — the opening fragment of a DT1
 * record too long for one frame. Previously carried as "the ASCII model-name
 * frame", which is what its one observed use holds. [EVIDENCED (image +
 * corpus) — the segmentation rule is FUN_0c003398 @0c003398 (S-1608); the
 * pairing is proved by the DT1 checksum, which closes only across both
 * fragments.]
 */
#define REAC_OP_DT1_FIRST_FRAGMENT      0x0401

/* The same record's closing fragment, carrying the inner checksum and the
 * SysEx terminator. Previously carried as "the extra cold-connect frame some
 * models send". [EVIDENCED (image + corpus).]
 */
#define REAC_OP_DT1_LAST_FRAGMENT       0x0402

/* A record container — a Roland DT1 SysEx, or the box's upstream return
 * block. Discriminated by payload[0]. [EVIDENCED (corpus).]
 */
#define REAC_OP_DT1_CONTAINER           0x0403

/* The op a cfea announce always carries. [EVIDENCED (corpus).] */
#define REAC_OP_ANNOUNCE                0xffff

/* ---- The control block's header, as the firmware builds it ---------------------
 * The two bytes at block[0:2] have always been read as one 16-bit op. The
 * stagebox firmware writes them as two independent fields, and the receive
 * side gates on them separately.
 *
 * EVIDENCED (image), S-1608 `FUN_0c003398` @0c003398 — the box's own
 * segmented upload, which writes block[0] = 1, block[4] = 0, then sets
 * block[1] to 3 when the whole body fits in one frame, to 1 for the first of
 * many, to 0 for a middle frame and to 2 for the last. The receiver agrees:
 * `FUN_0c003aae` @0c003aae gates on `buf[1] & 1` and `FUN_0c003b88`
 * @0c003b88 on `buf[1] in {0, 2}`.
 *
 * So there are not seven scene-and-record ops. There is a LINK selector, a
 * two-bit SEGMENT field, a length, and an opcode.
 */
/* block[0] — the link selector. [EVIDENCED (image) — every builder writes it
 * first.]
 */
#define REAC_HDR_LINK_OFF               0

/* block[1] — the segment flags. [EVIDENCED (image).] */
#define REAC_HDR_SEG_OFF                1

/* block[2:4] — the big-endian length. [EVIDENCED (image).] */
#define REAC_HDR_LEN_OFF                2

/* block[4] — the opcode, and the base the length counts from on every family
 * except a link-1 bulk transfer. [EVIDENCED (image) — FUN_0c002c70 @0c002c70
 * writes 25 for eight three-byte records at block[5:29], and 25 = 1 + 8*3.]
 */
#define REAC_HDR_OPCODE_OFF             4

/* block[1] bit 0 — the frame opens a transfer and carries its total.
 * [EVIDENCED (image).]
 */
#define REAC_SEG_FIRST_BIT              0x01

/* block[1] bit 1 — the frame closes a transfer. [EVIDENCED (image).] */
#define REAC_SEG_LAST_BIT               0x02

/* Both bits — a complete message in one frame, which is what every chanmap,
 * heartbeat, declaration and single-frame record is. [EVIDENCED (image +
 * corpus).]
 */
#define REAC_SEG_SINGLE                 0x03

/* The stagebox control link — the scene transfer, the chanmap, the heartbeat
 * and the declarations. [EVIDENCED (image + corpus).]
 */
#define REAC_LINK_CONTROL               0x01

/* A second link the S-4000S image builds for and the S-1608 image has no code
 * for at all. [EVIDENCED (image) — FUN_0c0128b8 @0c0128b8 (S-4000S). NEVER
 * OBSERVED on the wire.]
 */
#define REAC_LINK_AUX_S4000S            0x02

/* The record link — the Roland DT1 container and its two fragments.
 * [EVIDENCED (corpus).]
 */
#define REAC_LINK_RECORD                0x04

/* A link-1 bulk FIRST frame puts its declared total at block[5:7] and its
 * payload at block[7]. [EVIDENCED (image + corpus).]
 */
#define REAC_SEG_FIRST_PAYLOAD_OFF      7

/* Every other bulk frame puts its payload at block[5]. [EVIDENCED (image +
 * corpus).]
 */
#define REAC_SEG_CONT_PAYLOAD_OFF       5

/* 24 — the largest first-frame chunk, and exactly 31 - 7. The builder
 * reserves block[31] for the checksum. [EVIDENCED (image) — FUN_0c003398
 * @0c003398.]
 */
#define REAC_SEG_FIRST_MAX              24   /* 0x0018 */

/* 26 — the largest continuation chunk, and exactly 31 - 5. [EVIDENCED
 * (image).]
 */
#define REAC_SEG_CONT_MAX               26   /* 0x001a */

/* The block offset the length counts from, INCLUSIVE — for every class
 * and subtype except one. A sister branch published this as universal;
 * it is not, and the exception is the commonest record on the wire.
 * See RECLEN_BULK_EXCEPTION.
 *   [EVIDENCED (image + corpus).]
 */
#define REAC_CTRL_RECLEN_BASE           4

/* The one subtype whose length does NOT count from block[4]: the
 * session-class bulk transfer. Its length is THIS FRAGMENT'S PAYLOAD
 * ONLY, at block[7] on a FIRST fragment and block[5] on any other.
 *
 * The scene body is 8904 bytes and arrives as 0x18 + 341 x 0x1a + 0x0e
 * = 24 + 8866 + 14. Under a base of 4 the payloads would be 21 and 25,
 * and 8904 - 21 - 14 = 8869 is not a multiple of 25 — the transfer
 * cannot be reassembled under that reading at all. The box's own sender
 * passes the chunk size both to the length store and to the memcpy that
 * fills block+7 or block+5.
 *   [EVIDENCED (image, S-1608 FUN_0c003398 @0c003398) + the reassembly
 *   arithmetic, asserted both ways in spec/reac_xcheck.py.]
 */
#define REAC_RECLEN_BULK_EXCEPTION      0x00

/* ---- The chanmap's three-byte record and the table it writes -------------------
 * Each record is `{slot, cell_and_flags, sens}` and it is the same shape in
 * both
 * directions: the box's transmitter is the inverse of its receiver, cell for
 * cell. EVIDENCED (image), `FUN_0c002bb2` @0c002bb2 (build) and
 * `FUN_0c002d42` @0c002d42 (apply), S-1608; `FUN_0c014488` and `FUN_0c014674`
 * in the S-4000S carry the identical arithmetic.
 *
 * The third byte is not padding. It lands in the same table cell the box's
 * own gain path reads.
 */
/* slot, flags, value. [EVIDENCED (image + corpus).] */
#define REAC_CHANMAP_REC_BYTES          3

/* One window of the ring per frame. [EVIDENCED (image + corpus).] */
#define REAC_CHANMAP_RECS_PER_FRAME     8

/* 48 slots plus the one non-channel record id, which the box's cursor emits
 * as index 0x30 before wrapping to 0. [EVIDENCED (image + corpus).]
 */
#define REAC_CHANMAP_RING_LEN           49

/* 48 — the protocol slot space, the present-bit array length and the
 * fully-enrolled sum. Identical in the S-1608 and S-4000S images, so it is a
 * protocol constant and not a per-model one. [EVIDENCED (image, both boxes).]
 */
#define REAC_SLOT_SPACE                 48   /* 0x0030 */

/* The per-slot table's stride, in the chanmap apply, in the scene commit and
 * in the scene store alike. [EVIDENCED (image, both boxes).]
 */
#define REAC_SLOT_RECORD_STRIDE         10

/* 80 — the table is 80 records long though only the first 48 are addressable
 * from the wire. [EVIDENCED (image, both boxes).]
 */
#define REAC_SLOT_TABLE_RECORDS         80   /* 0x0050 */

/* Table offset the record's VALUE byte writes — the cell the gain path reads.
 * [EVIDENCED (image) — FUN_0c007fbc @0c007fbc feeds it to FUN_0c007e6a
 * @0c007e6a.]
 */
#define REAC_SLOT_CELL_VALUE            2

/* Table offset written from flags bit 3. [EVIDENCED (image).] */
#define REAC_SLOT_CELL_FLAG_BIT3        4

/* Table offset written from flags bit 1 — the one the group apply shadows and
 * never actuates. [EVIDENCED (image).]
 */
#define REAC_SLOT_CELL_FLAG_BIT1        6

/* Table offset written from flags bit 2. [EVIDENCED (image).] */
#define REAC_SLOT_CELL_FLAG_BIT2        8

/* The mask the builder ANDs with `cell << 4`, so the cell code is a full four
 * bits. Read out of the image at DAT_0c002d7c. [EVIDENCED (image).]
 */
#define REAC_CHANMAP_CELL_MASK          0xf0

/* The non-channel record whose FLAGS byte reaches the box's one-word identity
 * cell and its change detector. Read out of the image at DAT_0c002d7e and
 * DAT_0c002e6e. [EVIDENCED (image + corpus) — 3804 in the corpus.]
 */
#define REAC_CHANMAP_ID_IDENTITY        0xfe

/* The all-zero record the builder emits past the sentinel. Read out of the
 * image at DAT_0c002d80. [EVIDENCED (image). NEVER OBSERVED — the cursor
 * wraps at 0x30, so the branch is unreachable on that path.]
 */
#define REAC_CHANMAP_ID_FILLER          0xff

/* ---- The state-4 commit — what it flushes --------------------------------------
 * WHAT THE COMMIT IS. The box runs a scene FSM, S-1608 FUN_0c0037ee, over
 * one RAM cell. State 2 takes the master's opening scene fragment, state 3
 * reassembles the continuations, and state 4 is FUN_0c003c8a — the COMMIT.
 * Nothing else promotes staged state into the active table; the head-amp
 * apply FUN_0c007fbc reads the ACTIVE table and only the commit writes it
 * wholesale.
 *
 * THREE THINGS ARE TWELVE WIDE AND ONLY ONE OF THEM IS A GROUP FLUSH. This
 * was the open question and the firmware answers it: the commit touches TWO
 * of the three, and NEITHER of them is a phantom flush.
 *
 *   1. The slot table copy is EIGHTY wide, not twelve. staging 0x0c0cd52e
 *      -> active 0x0c0cf85a, fields +2 +4 +6 +8, `while (i < 0x50)`,
 *      unconditional — there is no per-slot enrolled test in the commit.
 *
 *   2. The twelve-wide loop `FUN_0c00f9aa(i, *(u16*)(staging + i*0x28))`
 *      reads field +0 of every FOURTH slot record (0x28 = four 10-byte
 *      records) and writes a 12-entry u16 array at 0x0c0f62fa. That array
 *      is the PEER INVENTORY MIRROR: what the master has declared each
 *      group of four channels to be. Its reader is FUN_0c00fa1a, which
 *      refuses to answer unless the link word is 1; its reset FUN_0c00fa58
 *      fills it with 3 = absent. The SAME array is written by the slot-map
 *      ingest FUN_0c002d42 from the high nibble of byte 1, at group anchors
 *      only. It touches no hardware.
 *
 *   3. The twelve bytes in the emitted report come from a DIFFERENT array —
 *      the box's OWN inventory, 12 records of 6 bytes at 0x0c080920, read
 *      through FUN_0c00f6ca and initialised by FUN_0c00f7f8. This is the
 *      declared inventory, and it is what reaches the wire.
 *
 * SO "THE COMMIT FLUSHES 12 PHANTOM GROUPS" IS WRONG in both halves. There
 * is no phantom in either twelve-wide structure; phantom reaches hardware
 * from the ACTIVE table through FUN_0c007fbc, in groups of EIGHT, ten of
 * them, and the commit does not call it. The two twelve-wide things the
 * commit does touch are both INVENTORY — one inbound, one outbound.
 *
 * Cross-checked on a second image: S-4000.BIN has the identical shape — the
 * 0x50-slot copy, the six-byte master id, the `i * 0x28` twelve-group loop
 * over its own staging table 0x0c0cd66a, and the same report.
 *
 * ON THE S-4000 ADDRESSES, because a base error would silently invalidate
 * these citations while leaving the conclusion right. They are quoted in the
 * address space of `devices/S-4000S/decompile/S-4000_alldecomp.c`, which is
 * 0x0C000000, and that space was CHECKED rather than assumed: all 832
 * PTR_FUN literal values in that image resolve exactly to addresses of
 * functions listed in the same decompilation. At a base of 0x0BFF0000 only
 * 20 of 832 resolve. Real code also begins at file offset 0x10 and the
 * listing's functions span file offsets 0x890..0x41e68, so there is no
 * unmapped 0x10000 prefix that would reconcile the two. File offsets, which
 * survive any re-basing: the commit at 0x13834, its literals at 0x1382c,
 * 0x13750, 0x13752.
 *
 * The RAM pointers these literals hold (0x0c0cd66a and the rest) are values
 * baked into the image and are correct as runtime addresses whatever base
 * the code is read at.
 */
/* Slots copied staging -> active by the commit, unconditionally. Eighty,
 * not forty-eight — the box's addressable channel space is 48 (the slot
 * map ingest gates slot < 0x30) but the table it lives in is 80 deep and
 * the head-amp apply walks ten groups of eight over it.
 *   [EVIDENCED (image, S-1608 FUN_0c003c8a + S-4000 same loop).]
 */
#define REAC_COMMIT_SLOT_TABLE_ENTRIES  80   /* 0x0050 */

/* Stride of the staging and active slot tables. Fields +2 sens, +4/+6/+8 the
 * three per-slot booleans; +0 is not copied and is read at four-slot stride
 * as the group's inventory cell. [EVIDENCED (image, S-1608 FUN_0c003c8a +
 * FUN_0c002d42).]
 */
#define REAC_COMMIT_SLOT_RECORD_BYTES   10

/* The granted master's identity, copied staging -> active by the commit.
 * Zeroed on link loss by FUN_0c003a64; a mismatch against the observed master
 * forces the FSM to state 0 (FUN_0c0045ec), which is why a takeover resets
 * the box's head-amp. [EVIDENCED (image).]
 */
#define REAC_COMMIT_MASTER_ID_BYTES     6

/* The head-amp hardware apply FUN_0c007fbc(bank, group) walks EIGHT
 * slots, `group << 3`, for group 0..9. This is the geometry that
 * actually reaches hardware, and it is a different axis from the
 * four-channel inventory cell. Confusing the two is how "twelve
 * phantom groups" got written down.
 *   [EVIDENCED (image, S-1608 FUN_0c007fbc).]
 */
#define REAC_HEADAMP_APPLY_GROUP_CHANNELS 8

/* Groups of eight the apply accepts, 0..9, covering the 80-slot active table.
 * [EVIDENCED (image, S-1608 FUN_0c007fbc).]
 */
#define REAC_HEADAMP_APPLY_GROUPS       10

/* ---- op-0103 sub-pages and subtypes --------------------------------------------
 * `payload[0]` — block[4], the byte a scene frame requires to be zero — is
 * the SUBTYPE SELECTOR, and it is the ONLY discriminator. That is how
 * `01 03 00 10` and a head-amp block came to be taken for one message.
 *
 * THE FOUR `PAGE_*` NUMBERS BELOW ARE LENGTHS, NOT SELECTORS, and this
 * group used to say otherwise. op_len is a plain record byte count on every
 * op — see the control_header group for the firmware that writes it. Each
 * subtype happens to have a fixed size today, so keying a parser on op_len
 * appears to work and then fails silently on the first short frame. Key on
 * the subtype; the lengths stay here because an emitter must still fill
 * op_len correctly, and they are named `LEN_` to say what they are.
 */
/* Record length of the master's slot-map window — the subtype byte plus
 * eight 3-byte slot records. S-1608 FUN_0c002c70 builds it.
 *   [EVIDENCED (corpus).]
 */
#define REAC_LEN_SUB_CHANMAP            0x0019

/* Record length of the box's state-4 commit report — the subtype byte,
 * two zero bytes, the board-configuration code, twelve inventory cells.
 * S-1608 FUN_0c003c8a builds it; S-4000 has the same shape.
 *   [EVIDENCED (corpus).]
 */
#define REAC_LEN_SUB_COMMIT_REPORT      0x0010

/* Record length of the master's prepare-to-grant frame. [EVIDENCED (image +
 * corpus).]
 */
#define REAC_LEN_SUB_ENROLL_GROUP_MAP   0x000d

/* Record length of the box's link-check ack — the subtype byte and
 * nothing else. S-1608 FUN_0c003fe2 builds it, from scene-FSM state 7.
 *   [EVIDENCED (corpus).]
 */
#define REAC_LEN_SUB_LINK_ACK           0x0001

/* Block offset of the subtype selector — payload[0]. [EVIDENCED (executed
 * trace).]
 */
#define REAC_SUB_0103_OFF               4

/* Subtype 0 — scene. A scene frame requires this byte to be zero and the box
 * refuses the frame otherwise. [EVIDENCED (executed trace, image).]
 */
#define REAC_SUB_0103_SCENE             0x00

/* Subtype 1 — the master's SLOT MAP, eight 3-byte records to a frame.
 * This group used to call it "head-amp" and reac.ksy calls the same
 * frame the chanmap; both names are half of it. The box's ingest
 * FUN_0c002d42 splits each record three ways: the high nibble of byte 1
 * is the inventory cell for the group this slot anchors and is consumed
 * only where (slot & 3) == 0; bits 3, 2 and 1 of byte 1 are three
 * per-slot booleans; byte 2 is the SENS step, and it lands in the same
 * active-table field the head-amp apply FUN_0c007fbc reads back. So it
 * IS a channel map and it DOES carry head-amp state.
 *   [EVIDENCED (image, S-1608 FUN_0c002d42 + FUN_0c002bb2).]
 */
#define REAC_SUB_0103_SLOT_MAP          0x01

/* Bit 7 of the subtype marks a record travelling BOX -> MASTER. Every
 * box-built subtype has it (0x81 the link ack, 0x80/0x82/0x83/0x84 the
 * commit report) and no master-built one does (0x00 scene, 0x01 slot
 * map, 0x10 enroll group map).
 *   [EVIDENCED (image + corpus).]
 */
#define REAC_SUB_REPLY_BIT              0x80

/* The box's link-check ack, S-1608 FUN_0c003fe2, from scene-FSM state 7. It
 * was called SLAVE_ANNOUNCE4 in the inherited dissector. [EVIDENCED (image +
 * corpus).]
 */
#define REAC_SUB_0103_LINK_ACK          0x81

/* The master's prepare-to-grant frame, once about 1.6 s before the grant
 * burst. [EVIDENCED (corpus).]
 */
#define REAC_SUB_0103_ENROLL_GROUP_MAP  0x10

/* The box's state-4 COMMIT REPORT — NOT a slave announce, which is what
 * reacdriver called it and what our dissector repeated until
 * 2026-08-23. S-1608 FUN_0c003c8a builds it after promoting staging to
 * active, and it is reached only from state 4 of the scene FSM
 * FUN_0c0037ee, i.e. after the master's scene transfer has completed.
 *
 * 0x82 IS THE S-1608'S NUMBER, NOT THE PROTOCOL'S, and a consumer must
 * not match on it. The builder picks between two model-specific
 * literals on a link-state test: S-1608 0x82 linked (DAT_0c00401c) and
 * 0x80 otherwise (DAT_0c00401e); S-4000 0x84 linked (DAT_0c013750) and
 * 0x83 otherwise (DAT_0c013752). What selects the second arm used to be
 * recorded here as UNEXPLAINED and now is not: on the S-1608 it is
 * FUN_0c00f9f2 returning something other than 1, which happens whenever
 * the peer-declared word is unset or the link word is not 1. Observed
 * on the wire: S-1608 0x82, S-0808 and S-4000S both 0x84. Match the
 * family with SUB_REPLY_BIT, not the literal.
 *   [EVIDENCED (image, S-1608 FUN_0c003c8a + S-4000 same shape; corpus, three
 *   box models).]
 */
#define REAC_SUB_0103_COMMIT_REPORT     0x82

/* The commit report's OTHER subtype. FUN_0c003c8a @0c003c8a picks
 * between two literal-pool bytes on a predicate, DAT_0c00401c = 0x82
 * and DAT_0c00401e = 0x80, both read out of the image; the 0x80 arm
 * also forces block[7] to zero. FIRMWARE-ONLY: no capture carries one,
 * so what selects it (FUN_0c00f9f2 @0c00f9f2) is unresolved.
 *   [EVIDENCED (image). NEVER OBSERVED.]
 */
#define REAC_SUB_0103_DECLARATION_ALT   0x80

/* The commit report as some boxes send it. Parsed by the same page; no S-1608
 * code emits it. [EVIDENCED (corpus).]
 */
#define REAC_SUB_0103_COMMIT_REPORT_83  0x83

/* The commit report from the S-0808 and S-4000S family. No S-1608 code emits
 * it either — the S-1608 image holds only 0x82 and 0x80. [EVIDENCED (corpus)
 * — 44 frames in 14 captures.]
 */
#define REAC_SUB_0103_COMMIT_REPORT_84  0x84

/* The SAME block position discriminates op-0403's two forms. [EVIDENCED
 * (corpus).]
 */
#define REAC_SUB_0403_OFF               4

/* A genuine Roland DT1 record. [EVIDENCED (corpus).] */
#define REAC_SUB_0403_DT1_RECORD        0x00

/* The box's upstream return block — 6.05 million of them in the corpus, and
 * the look-alike a naive DT1 dispatch acts on. [EVIDENCED (corpus).]
 */
#define REAC_SUB_0403_BOX_RETURN        0x02

/* ---- DT1 record tags -----------------------------------------------------------
 * The two bytes after the DT1 command byte select the register page, and
 * they are the discriminator INSIDE the record link the way the subtype is
 * the discriminator inside the session link. Nothing generated them before,
 * so a consumer that wanted to name a record had to spell the numbers
 * itself — which is the drift this file exists to stop.
 *
 * Read them with the class and fragment fields, never alone: the same tag
 * 0x0500 appears in a single-frame record at rec_len 0x0016 and 0x001a AND
 * split across the 0x0401 / 0x0402 fragment pair.
 */
/* The mark record a console emits around a head-amp sweep. [EVIDENCED
 * (corpus).]
 */
#define REAC_DT1_TAG_HEAD_MARK          0x0000

/* The join/cold-connect state record, body 06 00 XX 00. [EVIDENCED (corpus).] */
#define REAC_DT1_TAG_JOIN_GRANT         0x0100

/* The preamp command — {CH, PARAM, VALUE}. The only tag the classifier treats
 * as its own kind. [EVIDENCED (corpus + rig).]
 */
#define REAC_DT1_TAG_HEAD_AMP           0x0101

/* A box-state record in the cold-connect exchange. [EVIDENCED (corpus).] */
#define REAC_DT1_TAG_BOX_READY          0x0302

/* The identity page — the 6- and 10-byte inventory bodies, and the ASCII
 * model name carried across the 0x0401 / 0x0402 fragment pair. [EVIDENCED
 * (corpus).]
 */
#define REAC_DT1_TAG_IDENTITY           0x0500

/* ---- The identity page (DT1 tag 0x0500) ----------------------------------------
 * What a box says it IS, and the one record a console needs to put a box in
 * a menu with a model and a firmware version.
 *
 * The tag is only the HIGH half of a Roland four-byte DT1 address. The low
 * half rides in the record body, so ONE tag covers six records and a
 * consumer that reads 0x0500 as a single struct gets five of them wrong.
 * The console polls all six with RQ1 (command 0x11) in one burst at
 * establishment; the box answers with DT1 (0x12) only for the addresses it
 * implements.
 *
 * MEASURED over the whole corpus: 1005 complete records (438 RQ1 in 6 x 73,
 * 567 DT1 replies) plus 18 fragment pairs, 85 captures.
 *
 * The firmware version is the row that closes the loop with the outside
 * world: its four bytes are four DECIMAL DIGITS, and the number they spell
 * is the version of the Roland release package each image came from. Three
 * models, three independent matches — so the boxes in this corpus run the
 * firmware images this project holds, and no firmware-derived fact in this
 * repo needs a version qualifier.
 */
/* `addr_lo`, the low half of the address, opens every 0x0500 record
 * body. The tag carries the high half, so the full DT1 address is four
 * bytes and an emitter that writes only the tag addresses record
 * 0x0000 by accident.
 *   [EVIDENCED (corpus) — 1005 records, six distinct addr_lo.]
 */
#define REAC_IDENTITY_ADDR_LO_BYTES     2

/* The full DT1 address is four bytes — the 2-byte tag plus the 2-byte
 * addr_lo that opens every 0x0500 record body. An emitter that writes
 * only the tag addresses record 0x0000 by accident.
 *   [EVIDENCED (corpus + image) — S-1608.BIN carries a 12-byte stride DT1
 *   address table at file 0x53154..0x53994, 176 records of {4-byte address,
 *   u32le, u32le byte count}.]
 */
#define REAC_IDENTITY_ADDR_BYTES        4

/* The system firmware version, 4 bytes, ONE DECIMAL DIGIT PER BYTE,
 * most significant first, displayed by Roland as D.DDD.
 *   [EVIDENCED (corpus + vendor package). S-0808 01 00 00 03 = 1.003 vs
 * package s0808_sys_v1003; S-1608 02 02 00 00 = 2.200 vs
 * s1608_sys_ver2200; S-4000S 02 05 00 00 = 2.500 vs s4000_sys_ver2500.
 * S-1608.BIN corroborates itself twice from the inside: boot banner
 * `ECM42 BOOT Ver.2.200` at file 0x200 and the boot-menu version
 * literal `2.200` at 0x9254. Capture
 * captures/m200i-s0808-48k-mirror__m200-BIDIR-coldboot-2026-07-11.pcap
 * frame 3475.
 * ]
 */
#define REAC_IDENTITY_ADDR_FIRMWARE_VERSION 0x0000

/* The REAC PROTOCOL version the box speaks — 8 bytes, four u16be: a
 * reserved word (0 everywhere, undecoded) then MAJOR, MINOR, PATCH. The
 * console prints it `major.minorPP`, the patch zero-padded to two digits.
 * S-0808 00 00 00 01 00 00 00 00 = (0,1,0,0) -> 1.000; S-1608
 * 00 00 00 02 00 03 00 02 = (0,2,3,2) -> 2.302; S-4000S-3208
 * 00 00 00 02 00 01 00 02 = (0,2,1,2) -> 2.102. It is a DIFFERENT number
 * from the firmware version at 0x0000 and neither substitutes for the
 * other: the S-1608 runs firmware 2.200 and speaks REAC 2.302.
 *   [EVIDENCED (corpus + console display) — 274 replies, 5 distinct boxes,
 * 3 models, constant per model. The operator read an M-200i's identity
 * display on 2026-09-14: the S-1608 shows `REAC 2.302` beside `Firmware
 * 2.200`, and the S-4000S-3208 (bench units 0040abc40680 and
 * 0040abc408bc) shows `REAC 2.102` beside `Firmware 2.500`. The decode
 * reproduces both strings from the captured bytes. The S-0808's 1.000 is
 * PREDICTED — no display has been read for one. An earlier reading, that
 * the second u16 tracked the REAC port count, is a coincidence of three
 * models and is dropped.
 * ]
 */
#define REAC_IDENTITY_ADDR_REAC_VERSION 0x0600

/* The model name: 1 byte name_kind (0x01 on every observation) then a
 * FIXED 16-byte NUL-padded ASCII field. 17 bytes of payload do not fit
 * the 36-byte control block, which is the ONLY reason any REAC record
 * is fragmented — this record is the whole population of the 0x0401 /
 * 0x0402 pair.
 *
 * ONLY THE S-0808 IMPLEMENTS IT. The S-1608 and S-4000S never answer
 * this address, so a console cannot read their model as text and must
 * take it from the firmware version plus the config announce's declared
 * width.
 *   [EVIDENCED (corpus + image). 18 fragment pairs in 9 captures, all
 * S-0808, inner checksum closing only across both fragments (data sum
 * 358, 0x1a completes it to 0x80 mod 256). The image agrees with the
 * silence: S-1608.BIN's page-0x0500 address table uses
 * third-address-byte 0x00..0x08 only — there is no 0x10 or 0x11 entry.
 * ]
 */
#define REAC_IDENTITY_ADDR_MODEL_NAME   0x1000

/* The model-name field is fixed width and NUL-padded — "S-0808" plus ten
 * zeros. A reader that stops at the first NUL is right; one that takes all 16
 * bytes as the name is wrong. [EVIDENCED (corpus) — 18 reassembled records,
 * all identical.]
 */
#define REAC_IDENTITY_NAME_FIELD_BYTES  16

/* An RQ1 body is addr_lo plus ONE byte, the number of bytes wanted.
 * A reply MAY BE SHORTER than that: the S-0808 is asked for 9 bytes at
 * 0x1011 and returns 1. Short is normal, not an error.
 *   [EVIDENCED (corpus) — 438 RQ1 records, sizes 4, 8, 17 and 9; 19 one-byte
 *   replies at 0x1011.]
 */
#define REAC_IDENTITY_RQ1_SIZE_BYTES    1

/* ---- The scene push ------------------------------------------------------------
 * After link-up a desk pushes its scene to the box as one bounded transfer:
 * an op-0101 header declaring the total and carrying the body's first 24
 * bytes, 341 op-0100 chunks of 26, and an op-0102 final of 14.
 *
 *   24 + 341*26 + 14 = 8904 = 0x22c8
 *
 * The three sizes and the total are ONE fact, not four: the lengths a step
 * declares are what must sum to what the header declares. A body whose
 * lengths do not sum leaves the box waiting for bytes that never come.
 *
 * The box completes reassembly ONLY on the final chunk, and completion runs
 * its state-4 commit — the sole promoter of staged head-amp into the active
 * table. A transfer that stops short leaves the box in reassembly for the
 * life of the link.
 */
/* The declared total, 0x22c8. The box gates its header on this exact value.
 * [EVIDENCED (image, both sides) — the same constant resolved out of the
 * S-1608's own image and out of the master's.]
 */
#define REAC_SCENE_BYTES                8904   /* 0x22c8 */

/* Body bytes carried by the op-0101 header, at block[7:31]. [EVIDENCED (image
 * + corpus).]
 */
#define REAC_SCENE_HEAD_BYTES           24

/* Body bytes per op-0100, at block[5:31]. Its declared op_len. [EVIDENCED
 * (image + corpus).]
 */
#define REAC_SCENE_CHUNK_BYTES          26

/* Body bytes in the op-0102 final. 8880 mod 26 — a length like any other
 * chunk's, not a fixed block. [EVIDENCED (image + corpus).]
 */
#define REAC_SCENE_TAIL_BYTES           14

/* op-0100 count for a whole body. [EVIDENCED (corpus) — measured back-to-back
 * in 0.680 s on a real M-200i driving an S-1608.]
 */
#define REAC_SCENE_CHUNKS               341

/* Header + chunks + final. [derived.] */
#define REAC_SCENE_STEPS                343

/* Block offset of the op word. [EVIDENCED (image).] */
#define REAC_SCENE_OP_OFF               0

/* Block offset of the BE payload length. [EVIDENCED (image) — a big-endian
 * 16-bit store from the same variable the master passes to the memcpy that
 * fills the payload.]
 */
#define REAC_SCENE_LEN_OFF              2

/* Block offset of the reserved/subtype byte. Zero on every scene step; the
 * box refuses the frame otherwise. [EVIDENCED (image + executed trace).]
 */
#define REAC_SCENE_SUB_OFF              4

/* Block offset of a chunk's and the final's body bytes. [EVIDENCED (image).] */
#define REAC_SCENE_CHUNK_PAY_OFF        5

/* Block offset of the header's BE declared total. The header's payload
 * therefore starts two bytes later than a continuation's. [EVIDENCED (image)
 * — `movi20]
 */
#define REAC_SCENE_HEAD_TOTAL_OFF       5

/* Block offset of the header's 24 body bytes — which is why a capture of this
 * frame alone shows the ASCII "1234" that opens the body. [EVIDENCED (image +
 * corpus).]
 */
#define REAC_SCENE_HEAD_PAY_OFF         7

/* ---- What the box validates in the body, and what it reads ---------------------
 * The commit checks THREE four-byte tags and nothing else. Fail any one and
 * it promotes nothing and replies nothing, while the transfer still looks
 * complete from the wire. Two of the three ride MIDDLE chunks, so a body
 * whose interior is wrong fails silently.
 *
 * THE TAGS ARE A GATE, NOT A DESCRIPTION OF WHAT THE BOX USES. Measured on
 * hardware: a body of zeros carrying only the three tags PASSES the commit
 * and leaves the box reporting model=unknown with ZERO capture ports, where
 * a recovered body brings it up as s1608 with 16. The unvalidated 8892 bytes
 * are not free — a wrong value there is not rejected, it is acted on.
 */
/* "1234". Rides the op-0101 header. [EVIDENCED (executed trace) — 2 of 70
 * zeroed 128-byte windows break the commit, and they are the two containing a
 * tag.]
 */
#define REAC_SCENE_TAG_ID_OFF           0x000

/* "SYSP". Rides op-0100 chunk 32. [EVIDENCED (executed trace).] */
#define REAC_SCENE_TAG_SYSP_OFF         0x368

/* "SCEN". Rides op-0100 chunk 33. [EVIDENCED (executed trace).] */
#define REAC_SCENE_TAG_SCEN_OFF         0x37c

/* The master's own MAC, INSIDE the body. On-wire identity must equal the L2
 * source, so a master replaying a recovered body substitutes its own.
 * [EVIDENCED (image + corpus).]
 */
#define REAC_SCENE_MAC_OFF              0x340

/* The pace code (0 = 48 kHz, 1 = 96 kHz, 2 = 44.1 kHz), the same value as
 * cfea[19]; see CONSOLE_FIELD_GATES_RATE. One of only two fields an emitter
 * fills in. [EVIDENCED (corpus) — 8 of 8904 bytes vary across 27 real-desk
 * bodies over three desk generations and four box models.]
 */
#define REAC_SCENE_REVISION_OFF         0x014

/* The 80-slot declaration table. [EVIDENCED (image + corpus).] */
#define REAC_SCENE_SLOTS_OFF            0x01a

/* Slots the commit promotes — unconditionally, all of them, with the SAME
 * constant. A scene body therefore CANNOT address an individual channel.
 * [EVIDENCED (executed trace).]
 */
#define REAC_SCENE_SLOT_COUNT           80

/* The scene record stride. 26 mod 10 = 6 and gcd(26,10) = 2 is the whole of
 * the "period-10 probe rotation with a phase step of +6" that was read as a
 * master state for a year. [EVIDENCED (image + corpus).]
 */
#define REAC_SCENE_SLOT_BYTES           10

/* Value at +0x04 on every desk generation and against every box. The commit
 * branches on it. [EVIDENCED (image + corpus).]
 */
#define REAC_SCENE_UNIT_MAP_SELECT      0x0001

/* Value at +0x08 on every capture. What it selects is UNRESOLVED. [EVIDENCED
 * (image + corpus).]
 */
#define REAC_SCENE_MAP_A_ARG            0x0004

/* ---- Head-amp — the record, its wire encoding and its actuation ----------------
 * A head-amp record is a Roland DT1 inside op-0403: TAG 0x0101, then
 * {CH, PARAM, VALUE}. Two things about it are routinely got wrong.
 *
 * TWO AXES, NOT THREE GRANULARITIES. This group used to say "three
 * granularities under one name" and list per-channel, per-four and
 * per-eight side by side. Those are not three points on one scale: two of
 * them are WIRE ENCODING (which record carries a field) and one is HARDWARE
 * ACTUATION (how many channels one write switches). Reading them as one
 * list makes a consumer wrong about whichever it picks.
 *
 * AXIS 1 — WIRE ENCODING, what a record carries.
 *
 *   DT1 head-amp record {CH, PARAM, VALUE}, op-0403 TAG 0x0101
 *     PARAM 0x00 phantom, 0x01 pad, 0x02 sens — one channel per record,
 *     all three. NO per-four gate exists on this path anywhere in the
 *     image, and the wire agrees: measured 2026-08-23, three desk
 *     generations address phantom on channels that are not multiples of
 *     four, and a single-channel toggle names only its own channel. See
 *     HEADAMP_GRAN_PHANTOM_SHIFT, which that measurement moved from 2 to 0.
 *
 *   SLOT MAP record {slot, cell+flags, sens}, op-0103 subtype 0x01
 *     FUN_0c002d42 writes the sens byte and the three flag bits (bits 3,
 *     2, 1) for EVERY slot, unconditionally. Only the HIGH NIBBLE is
 *     per-four: the `(slot & 3) == 0` branch hands `flags >> 4` to
 *     FUN_0c00f9aa and FUN_0c004164, the two 12-entry INVENTORY arrays.
 *     So the per-four field in this record format is the INVENTORY CELL,
 *     not a head-amp parameter.
 *
 * AXIS 2 — HARDWARE ACTUATION, what one write switches. PER CHANNEL, for
 * all three parameters. FUN_0c007fbc(bank, group) sets its cursor to
 * `group << 3` and loops EIGHT times; inside the loop, each iteration
 * passes its own within-bank index (0..7) and that slot's own value to
 * FUN_0c00ac1e (phantom), FUN_0c00ac96 (pad) and FUN_0c007e6a ->
 * FUN_0c007e30 (sens). Each channel gets its own value and its own write.
 *
 * SO THERE ARE EXACTLY TWO GRANULARITIES, NOT THREE: per channel and per
 * eight. Per channel is the ACTUATION. Per eight is the REFRESH BANKING —
 * and the readback nibble is the same eight, not a third axis. See
 * HEADAMP_BANK_CHANNELS, which is now the single row for both.
 *
 * BANK AND GROUP ARE DIFFERENT AXES AND OUR DOCS HAVE MIXED THEM UP.
 * Pinned here, once:
 *
 *   GROUP 0..9, and `group == ch >> 3`. It selects WHICH EIGHT CHANNELS'
 *          DATA — an eight-slot window of the 80-slot active table.
 *   BANK selects WHICH EIGHT PHYSICAL PREAMPS receive it. It is NOT a
 *          subdivision of channel space and it does not index the active
 *          table.
 *
 * They are independent, which is why FUN_0c007fbc takes both. At least one
 * doc has them inverted — FUN_0c012162(k) returns a GROUP 0..9 while its
 * argument k is a BANK — so anything reading either word should check it
 * against this definition rather than against neighbouring prose.
 *
 * A NOTE ON A CORRECTION MADE TO THIS FILE'S OWN CORRECTION: the firmware
 * lane's first report said phantom "reaches hardware in groups of EIGHT".
 * That read the loop bound as the actuator width and is wrong in the same
 * shape as the per-four claim it was correcting. The loop is eight long;
 * the write inside it is one channel wide.
 *
 * A RECORD WRITES THE ACTIVE TABLE, AND THE COMMIT OVERWRITES IT. There is
 * no head-amp staging table. So records must FOLLOW the commit, never
 * precede it: a record sent before it is erased, silently — the bytes are
 * right, the checksums are right, the box acknowledges, and the value is
 * gone.
 */
/* The DT1 register page for head-amp. [EVIDENCED (corpus).] */
#define REAC_HEADAMP_TAG                0x0101

/* +48V, value 0 or 1. WHICH BIT IS PHANTOM AND WHICH IS PAD IS OUR NAME, NOT
 * THE FIRMWARE'S — the mapping comes from the corpus and the rig, explicitly
 * NOT from the image. [EVIDENCED (corpus + rig).]
 */
#define REAC_HEADAMP_PARAM_PHANTOM      0x00

/* -20 dB pad, value 0 or 1. [EVIDENCED (corpus + rig).] */
#define REAC_HEADAMP_PARAM_PAD          0x01

/* Sensitivity step. [EVIDENCED (corpus + rig).] */
#define REAC_HEADAMP_PARAM_SENS         0x02

/* 55 — the 56th and last entry of the box's own step table, which is exactly
 * 56 rows with no spares. What each step is WORTH in dB is the headamp_sens
 * group below; this row is only the count. [EVIDENCED (image) — a 56-entry
 * table at 0x0c0327a0 in the S-1608 image, reached by both write paths,
 * ending exactly where the "V03.05" version string begins — in the S-1608
 * image; the S-0808 copy is a byte match only. The count is what that table
 * proves; reading a gain curve off its stage structure did not survive the
 * rig.]
 */
#define REAC_HEADAMP_SENS_MAX           0x37

/* 48 addressable wire channels, 0x00..0x2f. NOT the 40-slot audio fabric — a
 * table bounded by 40 silently rejects a 16-input box based at 0x20, so its
 * inputs 9..16 can never be given phantom, pad or sens. [EVIDENCED (corpus).]
 */
#define REAC_HEADAMP_CH_SPAN            0x30

/* SENS is per channel — ch >> 0. [EVIDENCED (executed trace).] */
#define REAC_HEADAMP_GRAN_SENS_SHIFT    0

/* The flag bits are per channel — ch >> 0. [EVIDENCED (executed trace).] */
#define REAC_HEADAMP_GRAN_FLAGS_SHIFT   0

/* PHANTOM IS PER CHANNEL — ch >> 0, exactly like pad and sens. This row
 * read 2 and was DISPUTED; it was measured on 2026-08-23 and the 2 is
 * withdrawn.
 *
 * THE MEASUREMENT. Taken on the live rig 2026-08-23 as phantom-test.pcap
 * and filed as
 * reacpw-s1608-48k-clean__phantom-ch24-on-off-2026-08-23.pcap in
 * ~/Devel/audio/reac-captures-raw/ (it still needs a MANIFEST row and a
 * distillation pass before it joins the committed corpus):
 * interface enp131s0 (the S-1608 segment), ethertype 0x8819 only,
 * snaplen 200, twelve seconds — idle, then phantom set TRUE on head-amp
 * CH 0x24, four seconds, then phantom set FALSE on 0x24, nothing else
 * touched. 0x24 is box port 5 and the first of the group of four
 * 0x24..0x27 that the retracted reading claimed one record covers.
 * Exactly TWO head-amp records crossed the wire in those twelve seconds
 * and both name CH 0x24 ALONE. 0x25, 0x26 and 0x27 never appear. Every
 * frame was truncated by the snaplen, so that is a claim to check rather
 * than assume: the control block is [18:50] and the record inside it
 * [34:40], both inside the 200 bytes, and all 38 control blocks in the
 * capture pass the block checksum while both head-amp records pass the
 * nested record checksum. Truncation removed audio and nothing the
 * verdict rests on.
 *
 * THE DISCRIMINATOR THIS ROW USED TO ASK FOR DOES NOT EXIST, which is
 * what had kept it stuck. The old note said: write 0x24, write 0x25, and
 * read the box's own RE-BROADCAST back. THERE IS NO RE-BROADCAST. The
 * S-1608 sent 47122 frames in that capture with ZERO dropped — its
 * 16-bit frame counter steps by one across all 47121 intervals — and
 * every one of them is either an audio frame, whose 16-slot descriptor
 * area is a constant `00 7a` per slot and did not move a byte at either
 * toggle, or one of twelve bare link-1 opcode-0x81 heartbeats whose
 * 32-byte block is byte-identical every time. The box volunteers no
 * head-amp state whatsoever. Neither does it to a real desk: in
 * m200-ch7-ON-OFF-ON-20260721-215743.pcap this same box answers an
 * M-200i toggling phantom six times with nothing but its heartbeat. A
 * consumer must therefore treat head-amp as WRITE-ONLY on this wire and
 * keep its own model; there is nothing to query and compare against.
 *
 * SO THE ROW IS SETTLED ON THE AXIS IT ACTUALLY GOVERNS — what a SENDER
 * puts on the wire, since a consumer that trusts a 2 emits phantom on
 * multiples of four only. Our own master emitting one record for 0x24
 * shows what OUR encoder does and nothing about Roland's law, so the
 * corpus supplied the desks. Across 3651 phantom records from three desk
 * generations — M-200i 00:40:ab:c9:cc:03, M-300 00:40:ab:c9:d8:5b,
 * M-5000 00:40:ab:ca:15:4c — 2304 address a channel that is NOT a
 * multiple of four. A full S-1608 sweep names 0x20..0x2f, all sixteen;
 * an S-0808 names 0x00..0x07; an S-4000S names 0x00..0x1f. The decisive
 * single case is m200-ch7-ON-OFF-ON: a real M-200i toggling ONE
 * channel's phantom on and off three times, six records, every one of
 * them CH 0x26. 0x26 & 3 == 2, so a desk obeying "per four" would have
 * had to write 0x24 and could not have expressed that toggle at all.
 *
 * THE READING THAT LOSES, AND WHY IT WAS BELIEVED. It was "phantom is
 * per group of FOUR, ch >> 2", graded EVIDENCED (executed trace). That
 * grade is why the static case was never allowed to flip this row, no
 * matter how strong it got: inference does not overturn an observation.
 * It is overturned here by MORE observations — more desks, more records,
 * and a purpose-built single-channel toggle on a channel that is not a
 * group leader. The static read turns out to have been right the whole
 * time: there is no per-four gate on the DT1 path anywhere in
 * S-1608.BIN. The single channel-indexed `(x & 3) == 0` test in that
 * image is FUN_0c002d42's, it gates `flags >> 4` into the two 12-entry
 * INVENTORY arrays, and it sits on the SLOT-MAP path, not the head-amp
 * path. The per-four number that is real belongs to PORTS_CH_PER_SLOT,
 * the inventory cell, and the likeliest history is that a trace of the
 * CELL moving was read as phantom moving. The capture is consistent with
 * that: the master's slot map reported slot 0x24 after the ON and again
 * after the OFF, flags 0x28 and sens 0x00 both times, and across all 48
 * channel slots it recorded zero state changes in the whole twelve
 * seconds. A phantom write does not touch the slot map.
 *
 * WHAT IS STILL NOT MEASURED, so nobody reads more into this than it
 * says: HARDWARE ACTUATION. No capture can show whether energising 0x24
 * also energises 0x25..0x27 inside the box, because the box reports
 * nothing back. That axis is HEADAMP_ACTUATION_SHIFT; it reads 0 from
 * the image, and confirming it needs a physical 48 V measurement on box
 * inputs 6, 7 and 8 while only input 5 is written — never a soft
 * indicator.
 *   [EVIDENCED (executed trace) —
 *   reacpw-s1608-48k-clean__phantom-ch24-on-off-2026-08-23.pcap (taken as
 *   phantom-test.pcap), enp131s0, snaplen 200 (one record for CH 0x24 alone
 *   per toggle; the box re-broadcasts nothing, 0 dropped frames), together
 *   with 2304 non-group-leader phantom records from three desk generations in
 *   the corpus and m200-ch7-ON-OFF-ON-20260721-215743.pcap as the decisive
 *   single-channel case. OVERTURNS the earlier executed trace that read 2,
 *   and vindicates the image read (S-1608 FUN_0c002d42 gates the inventory
 *   cell, not phantom).]
 */
#define REAC_HEADAMP_GRAN_PHANTOM_SHIFT 0

/* HARDWARE ACTUATION IS PER CHANNEL, for phantom, pad and sens alike.
 * FUN_0c007fbc's loop passes a per-channel index and that slot's own
 * value to each of the three writers on every one of its eight
 * iterations. This is the number that says what one write switches, and
 * it is the axis the three old "granularity" rows never had.
 *   [EVIDENCED (image, S-1608 FUN_0c007fbc -> FUN_0c00ac1e / FUN_0c00ac96 /
 *   FUN_0c007e6a).]
 */
#define REAC_HEADAMP_ACTUATION_SHIFT    0

/* THE PER-EIGHT GRANULARITY, and there is only one of them. This row
 * folds together what used to be two — "the hardware bank" and "the
 * readback nibble is per eight" — because they are arithmetically the
 * same thing and were being read as two independent pieces of evidence.
 *
 * FUN_0c007fbc sets its cursor to `group << 3` and loops exactly eight
 * times, so group g covers [g*8, g*8+8) and therefore `g == ch >> 3` —
 * which is the readback nibble's own index. One axis, two spellings:
 * this width 8 and HEADAMP_BANK_SHIFT's 3.
 *
 * It is a REFRESH AND READBACK banking, not an actuation width: the
 * writers take (bank, 0..7) and each of the eight iterations writes one
 * channel. Two banks of eight cover an S-1608's sixteen analog inputs.
 *   [EVIDENCED (image) — S-1608 FUN_0c007fbc for the loop and the per-channel
 *   writes; its caller, recovered 2026-08-23 from a function Ghidra never
 *   disassembled (clean prologue past the previous function's rts, absent
 *   from the 1395-entry map, reached by a plain bsr), for `group == ch >> 3`.
 *   Supersedes the earlier UNRESOLVED note that the caller could not be
 *   traced.]
 */
#define REAC_HEADAMP_BANK_CHANNELS      8

/* `ch >> 3` gives the bank/group index. THE SAME FACT AS
 * HEADAMP_BANK_CHANNELS above, spelled as a shift instead of a width —
 * consult one or the other, never both as corroboration.
 *
 * It was called HEADAMP_GRAN_READBACK_SHIFT and sat beside the bank row
 * as if the readback were a third, independent granularity. It is not:
 * the apply loop's group index and the readback nibble are the same
 * `ch >> 3` over the same 80 slots.
 *   [EVIDENCED (executed trace for the readback nibble; image for the apply
 *   loop's identical index — S-1608 FUN_0c007fbc and its caller). The two
 *   agree, which is why they are one row.]
 */
#define REAC_HEADAMP_BANK_SHIFT         3

/* Every real desk sweep is ONE contiguous pass over the box's full declared
 * width, every channel getting all three parameters — 24 records for an
 * S-0808, 48 for an S-1608, 96 for an S-4000S. No desk addresses a bank,
 * splits a sweep or repeats one. [EVIDENCED (corpus) — 31 of 47 captures,
 * three desk generations agreeing on the same box.]
 */
#define REAC_HEADAMP_SWEEP_RECORDS_PER_CH 3

/* A box's head-amp CH base is its OWN property, announced, never granted. The
 * config
 * announce `01 03 00 10` carries it at buf[7], and the master addresses the
 * box at
 * base = buf[7] * 0x10. An 8-input and a 32-input box are BOTH addressed at
 * 0x00, which
 * is what rules out an allocation: nothing in the box consumes a granted
 * base.
 *   [RESOLVED (firmware + corpus) — S-1608.BIN (SH-4 LE, base 0x0BFE0000):
 *   FUN_0c003c8a at
 * 0x0c003c8a sets buf[7] = FUN_0c00f6a8() = *0x0c080918. Corroborated on 29
 * captures in
 * reac-captures/analysis/placement_table.csv (S-0808 0x00->0x00, S-1608
 * 0x02->0x20,
 * S-4000S 0x00->0x00) across M-200i, M-300 and M-5000.
 * ]
 */
#define REAC_HEADAMP_BASE_FROM_CONFIG_BYTE7 1

/* base = config-announce buf[7] * 0x10. Sixteen head-amp rows per strap step,
 * which is
 * two 8-slot groups — the same unit the box applies in.
 * Exercised at exactly two points (0 and 2): firmware-grade for the S-1608,
 * corpus-grade
 * for the others.
 *   [RESOLVED (firmware + corpus) — same provenance as
 *   HEADAMP_BASE_FROM_CONFIG_BYTE7.
 * ]
 */
#define REAC_HEADAMP_BASE_MULTIPLIER    0x10

/* A master cannot move where a head-amp write lands by granting differently.
 * The box's
 * fabric-row-to-preamp map is the group number FUN_0c012162 returns, and
 * every input to
 * it is a GPIO strap or a fitted-board inventory. Retires reac-pw's
 * docs/PLACEMENT-EVIDENCE.md five-run rig experiment: declared width was
 * collinear with
 * the base only because a 16-in chassis always straps 2.
 *   [RESOLVED (firmware) — S-1608.BIN: FUN_0c0081f6 at 0x0c0081f6 reads
 *   *0x0c080918 and
 * applies FUN_0c007fbc(bank, FUN_0c012162(k)); FUN_0c007fbc at 0x0c007fbc
 * indexes the
 * head-amp table at 0x0c0cf85a by group*8 + i; the config record is built by
 * FUN_0c0123c0
 * (0x0c0123c0), called only from FUN_0c0052d4 (0x0c0052d4) with constants,
 * and the group
 * table at +0x78 has exactly three writers — FUN_0c0119ac, FUN_0c011bbc,
 * FUN_0c011e2c —
 * none of which reads a frame.
 * ]
 */
#define REAC_HEADAMP_BASE_IS_CHASSIS_NOT_GRANT 1

/* The box applies head-amp in groups of eight fabric rows, one 8-port board
 * at a time,
 * passing the within-group index 0..7 to the preamp. Anything reasoning about
 * head-amp
 * reach reasons in groups of 8 from the box's base, never per channel — note
 * this is the
 * APPLY unit and is a different axis from actuation, which is per channel.
 *   [RESOLVED (firmware) — S-1608.BIN: FUN_0c007fbc at 0x0c007fbc, slot =
 *   group << 3, eight
 * iterations.
 * ]
 */
#define REAC_HEADAMP_APPLY_UNIT_SLOTS   8

/* ---- The SENS step -> sensitivity curve ----------------------------------------
 * One decibel per step, over all 56 steps, with no duplicates anywhere.
 * Sensitivity runs -10 dBu at 0x00 down to -65 dBu at 0x37 with the pad off,
 * and the pad shifts the whole travel up by 20 dB:
 *
 *   sensitivity_dBu = -10 - value + (pad ? 20 : 0)
 *
 * MEASURED 2026-08-23, S-0808, output 1 cabled to input 1 so the source is an
 * electrical loopback of a level we generated and therefore know — not a
 * microphone in a room, which is what produced the third and wildest of the
 * readings this replaces. All 56 steps at three generator levels whose ranges
 * overlap and agree to 0.05 dB where they meet. Span 54.60 dB against the
 * 55.00 a flat 1 dB implies; least-squares slope 0.988 dB/step with a maximum
 * residual of 0.44 dB, which is the size of the measurement's own scatter, so
 * the law declared here is the round decibel and not the fitted 0.988. The pad
 * measured 20.12 and 20.20 dB at two different steps — the check that this dB
 * axis is the box's own.
 *
 * THE FIRMWARE HOLDS THE CURVE AS A TABLE, and here it is. S-1608.BIN at
 * 0x0c0327a0, 56 entries of two bytes, read by FUN_0c007e30 which clamps
 * the step to 0x37 and hands the pair to the preamp writer FUN_0c00af2a.
 * In the S-1608 image the table ends exactly where the ASCII version banner
 * "V03.05" begins, which is how we know its extent is 56 and not a run of
 * padding.
 *
 * The same 112 bytes appear in S-0808.BIN at file offset 0x45ec8. THAT IS A
 * RAW BYTE MATCH AND NOTHING MORE: there is no S-0808 decompilation, the
 * image is not a code image loadable at the S-1608's base, and its own
 * pointers are a different width and order (big-endian 0x0003xxxx). The
 * banner control above does NOT apply there — S-0808 has zero padding after
 * the table, not a banner. Nothing in this group is sourced from S-0808
 * disassembly; the byte match corroborates the table's content across two
 * models and the reading of it comes from the S-1608 alone.
 *
 *   step stage fine step stage fine step stage fine step stage fine
 *   0x00 3 0x00 0x0e 2 0x0c 0x1c 1 0x08 0x2a 0 0x04
 *   0x01 3 0x02 0x0f 2 0x0e 0x1d 1 0x0a 0x2b 0 0x06
 *   0x02 3 0x04 0x10 2 0x10 0x1e 1 0x0c 0x2c 0 0x08
 *   0x03 3 0x06 0x11 2 0x12 0x1f 1 0x0e 0x2d 0 0x0a
 *   0x04 3 0x08 0x12 2 0x14 0x20 1 0x10 0x2e 0 0x0c
 *   0x05 3 0x0a 0x13 2 0x16 0x21 1 0x12 0x2f 0 0x0e
 *   0x06 3 0x0c 0x14 2 0x18 0x22 1 0x14 0x30 0 0x10
 *   0x07 3 0x0e 0x15 2 0x1a 0x23 1 0x16 0x31 0 0x12
 *   0x08 2 0x00 0x16 2 0x1c 0x24 1 0x18 0x32 0 0x14
 *   0x09 2 0x02 0x17 2 0x1e 0x25 1 0x1a 0x33 0 0x16
 *   0x0a 2 0x04 0x18 1 0x00 0x26 1 0x1c 0x34 0 0x18
 *   0x0b 2 0x06 0x19 1 0x02 0x27 1 0x1e 0x35 0 0x1a
 *   0x0c 2 0x08 0x1a 1 0x04 0x28 0 0x00 0x36 0 0x1c
 *   0x0d 2 0x0a 0x1b 1 0x06 0x29 0 0x02 0x37 0 0x1e
 *
 * HOW TO READ IT. The two bytes are not decibels — they are two hardware
 * registers. `stage` is a two-bit coarse range: FUN_0c00af2a drives it onto
 * a pair of GPIO pins per channel, so it is an analog range switch, and
 * FUN_0c007f20 compares ONLY this byte between two steps, which is a
 * "does this change need the range to move" test. `fine` is a serial gain
 * code, clamped to 0x24 by the writer, and it steps by exactly 2 for every
 * step of the SENS index, everywhere in the table with no exception.
 *
 * WHAT THE TABLE PROVES, AND WHAT IT DOES NOT. It proves the shape: the
 * step is UNIFORM within a range, so any departure from a flat law can only
 * live at the three range breaks, which fall between steps 0x07/0x08,
 * 0x17/0x18 and 0x27/0x28. It does not carry a decibel — the dB per fine
 * LSB and the dB of each range tap are analog component values, in the
 * preamp and not in the image.
 *
 * The stages begin at index 0, 8, 24 and 40, and the fine field restarts at
 * zero at each. That placement is itself a statement: a break sits exactly
 * where the fine field would run out, so the designer intended the ranges
 * to abut with no gap and no overlap. Take the fine LSB as the usual half
 * decibel and the whole table reads out as gain_dB = step, 0 through 55,
 * with the range taps at 0, 8, 24 and 40 dB. That is the round law, and the
 * table is its decomposition into two registers.
 *
 * SO THE LAW IS THE FIRMWARE'S TABLE AND 54.60 IS A MEASUREMENT OF IT. The
 * two are not rival claims about the same quantity. The deficit is 0.40 dB
 * over 55 steps against a sweep whose own maximum residual was 0.44 dB, so
 * the measurement cannot separate 54.60 from 55.00 and does not contradict
 * it. What the rig DID settle, and the table could not, is that the three
 * range breaks are continuous: A/B/A gave +0.92/+1.12, +1.36/+1.31 and
 * +0.97/+0.84 dB at exactly the three indices the table puts them at. That
 * is the firmware's structure and the rig's numbers agreeing on the same
 * three places, which is the strongest form this fact can take.
 *
 * WHAT THIS SETTLES, because it was the schema's one openly contested number.
 * Three readings were live: reac.ksy and reac-pw both spelled the flat 1 dB
 * law, which is how a number nobody had measured came to look confirmed by two
 * sources; libreac carried a 56-entry firmware curve spanning 48.75 dB whose
 * stage breaks at 8, 24 and 40 made three pairs of steps deliver IDENTICAL
 * gain, so the map was not injective and a round trip through it was a
 * different function; and a rig measurement of 1.235 dB/step, since shown to
 * be an artefact of its acoustic source.
 *
 * The twins were the discriminating test and they were run: each pair by rapid
 * A/B/A alternation, twice, at two generator levels, so residual drift shows
 * as a mismatch between the A readings.
 *
 *   7 -> 8 +0.92 and +1.12 dB drift control 0.08 / 0.10 dB
 *   23 -> 24 +1.36 and +1.31 dB drift control 0.34 / 0.15 dB
 *   39 -> 40 +0.97 and +0.84 dB drift control 0.26 / 0.08 dB
 *
 * Every pair steps by about a decibel, an order of magnitude outside its own
 * control. The firmware's four coarse stages are real; gain being continuous
 * across their breaks was an inference from that structure and it is refuted.
 *
 * THE ANCHOR, stated honestly because half of it is not measured here. A
 * loopback measures the SPAN exactly — 54.60 dB between the endpoints, which
 * is what discriminates 48.75 from 55.00 — but the absolute dBu of either
 * endpoint needs one constant this experiment cannot separate: the box's own
 * converter reference, the dBu it puts out at 0 dBFS and the dBFS its
 * sensitivity spec refers to. The loop measures their SUM. So -10 dBu at step
 * 0 is carried over unchanged from every source that already agreed on it, and
 * -65 at 0x37 is what the measured span then makes it. Raw data:
 * reac-pw docs/measurements/sens-sweep-2026-08-23-*.csv.
 *
 * REMOVED 2026-09-25 as dead facts
 * (docs/audits/2026-09-25-contract-copies.md):
 * four rows describing the firmware's analog STAGES, read by no consumer,
 * no grammar and no copy in any of the three repos. What they recorded:
 * the step table selects between 4 coarse analog ranges, driven onto two
 * GPIO pins per channel by FUN_0c00af2a (EVIDENCED image), whose first
 * steps are 0x08, 0x18 and 0x28; those three breaks are the only places a
 * uniform step could fail, and the rig measured all three at about a
 * decibel (EVIDENCED image + rig) - which is what HEADAMP_SENS_STEP_CDB
 * rests on. Restore them from git history if a consumer ever needs them.
 */
/* Step 0x00 with the pad off, in hundredths of a dBu — the least sensitive
 * setting and the reference the whole travel hangs off. [INFERRED — agreed by
 * every prior reading and not independently measurable through a loopback,
 * which sees only this plus the box's converter reference. The SPAN below is
 * what was measured.]
 */
#define REAC_HEADAMP_SENS_REF_CDB       -1000

/* One decibel, every step, all 55 transitions. The number that was disputed,
 * and the one thing a consumer cannot get wrong quietly. [EVIDENCED (image +
 * rig). Image — the S-1608 table at 0x0c0327a0 steps
 * its fine register by exactly 2 for every SENS step with no exception,
 * so the law is uniform inside each of its four ranges and can only
 * break at three indices. Rig — the 2026-08-23 loopback sweep of all 56
 * steps, span 54.60 dB, slope 0.988, and A/B/A at each of those three
 * indices giving about a decibel against controls of 0.08 to 0.34 dB.
 * The 0.40 dB the span falls short is inside that sweep's own 0.44 dB
 * maximum residual, so it does not stand against the table.
 * ]
 */
#define REAC_HEADAMP_SENS_STEP_CDB      100

/* Entries in the firmware's SENS table, steps 0x00..0x37. The writer
 * FUN_0c007e30 clamps anything above 0x37 to 0x37, so 0x37 is the top of the
 * travel and not merely the last one observed. [EVIDENCED (image, S-1608
 * 0x0c0327a0 = file 0x527a0; the same 112 bytes at S-0808 file 0x45ec8 as a
 * raw byte match, not a disassembly — there is no S-0808 decompilation).]
 */
#define REAC_HEADAMP_SENS_STEPS         56

/* The pad's 20 dB, in hundredths, added to the sensitivity when it is on.
 * Independent of the step, and it earns its place here by being the one
 * number in this group whose absolute value the loopback DOES measure.
 * [EVIDENCED (rig) — 20.12 and 20.20 dB by A/B/A at steps 0x37 and 0x28,
 * against pad-off controls of 0.11 and 0.16 dB.]
 */
#define REAC_HEADAMP_PAD_CDB            2000

/* ---- Head-amp base per declared width — derived, not the law -------------------
 * A head-amp record's CH is base + (box_input - 1). The base is
 * `HEADAMP_BASE_FROM_CONFIG_BYTE7 * HEADAMP_BASE_MULTIPLIER` (see the
 * `head_amp` section above) — a wire field, the box's own config-announce
 * `buf[7]`, a GPIO chassis strap read before the RTOS starts. It is not
 * negotiated session state and a master cannot move it by granting
 * differently.
 *
 * The table below still reads correctly for the three chassis we own
 * because declared width is collinear with the strap on all three; it is
 * not the base's carrier. An 8-input and a 32-input box are both strapped
 * (and so both based) at 0x00, which is what rules out width as the law.
 */
#define REAC_PLACEMENT_ROWS 3
/* { in_ch, base } rows: */
#define REAC_PLACEMENT_TABLE { { 8, 0x00 }, { 16, 0x20 }, { 32, 0x00 } }
/* 8 -> 0x00: S-0808, strap 0x00. [EVIDENCED (corpus) — 42 grant sweeps across
 * 82 captures.]
 */
/* 16 -> 0x20: S-1608, strap 0x02. The one width that is not zero, and the
 * reason a 40-bounded table drops half the box. [EVIDENCED (corpus).]
 */
/* 32 -> 0x00: S-4000S, strap 0x00. Wider than the S-1608 and still based at 0
 * — which is what kills "lowest fit" and "top alignment" as candidate laws.
 * [EVIDENCED (corpus).]
 */

/* ---- The config-announce port table --------------------------------------------
 * A box declares its own geometry: twelve cells of four channels each,
 * spanning the 48-channel fabric ring. Geometry comes from the declaration,
 * not from a hand-kept model list.
 */
/* Block-relative start of the twelve-cell table. [EVIDENCED (corpus).] */
#define REAC_PORTS_TABLE_OFF            8

/* 12 x 4 = the 48-channel fabric. [EVIDENCED (corpus).] */
#define REAC_PORTS_TABLE_SLOTS          12

/* Channels per cell. [EVIDENCED (corpus).] */
#define REAC_PORTS_CH_PER_SLOT          4

/* A 4-output group. [EVIDENCED (corpus).] */
#define REAC_PORT_SLOT_OUT              0x01

/* A 4-input group. [EVIDENCED (corpus).] */
#define REAC_PORT_SLOT_IN               0x02

/* An empty cell. [EVIDENCED (corpus).] */
#define REAC_PORT_SLOT_EMPTY            0x03

/* A 4-input group, as the S-4000H (8 in / 32 out) declares its inputs.
 * Captured only on that chassis (MAC 00:40:ab:c4:25:80); a code this decoder
 * used to REFUSE THE WHOLE TABLE on, which kept a fully declaring box off the
 * graph for minutes (operator ruling 2026-09-17, "we should be able to enrol
 * any stage box"). WHAT SEPARATES 0x00 FROM 0x02 IS NOT DECIDED — one
 * chassis, one capture; a splitter's inputs may be marked apart from a
 * head-amp-owned input, or this may be nothing but an alternate spelling of
 * `analog_input`. [EVIDENCED (corpus, m200-s4000h-coldboot.pcap +
 * m200-s4000h-replug.pcap, box 00:40:ab:c4:25:80, cells = 01x8 00x2 03x2).]
 */
#define REAC_PORT_SLOT_IN_SPLIT         0x00

/* A cell carrying a code outside {00,01,02,03} is no longer a reason to
 * refuse the table. A SLOT CODE SPEAKS FOR ITS OWN FOUR CHANNELS AND NO
 * OTHERS — the box enrols at the width this decoder can read, and the unknown
 * group is reported (`reac_ports_unknown`), never guessed into either role.
 * No such code has been captured; this is the refusal rule for the day one
 * arrives. [EVIDENCED (libreac reac_ports.c; no unknown code seen in the
 * corpus).]
 */
#define REAC_PORT_SLOT_UNKNOWN_COST     1

/* OPEN, not a law. The S-4000H straps board_config_code = 0x00 like the
 * S-0808 and the S-4000S, predicting head-amp base CH 0x00 by `base =
 * board_config_code * 0x10`. An M-200 has been observed addressing this box's
 * preamps at CH 0x20–0x27 instead — a rig check, not a capture, and not yet
 * reconciled with the strap law. Do not derive a base for a 0x00-coded box
 * from the strap alone until this is resolved. [OPEN (rig observation,
 * unreproduced in a capture).]
 */
#define REAC_PORT_SLOT_IN_SPLIT_HEADAMP_BASE 1

/* One per enrolled input group of 8, front-packed. [EVIDENCED (image +
 * corpus).]
 */
#define REAC_ENROLL_GROUP_IN            0x41

/* One per non-input group, back-packed. [EVIDENCED (image + corpus).] */
#define REAC_ENROLL_GROUP_OUT           0xc3

/* Five groups of 8 span exactly the 40-slot audio fabric. [EVIDENCED (image +
 * corpus).]
 */
#define REAC_ENROLL_GROUPS              5

/* ---- Frame field widths the geometry is built from -----------------------------
 * The widths the header offsets above are sums of. They were literals in
 * the `derived:` expressions of this very file (`2 + CTRL_BLOCK_LEN`) and
 * in every consumer that computed an offset (`frame + 16`, `50 + 2`), so
 * the offsets are now derived from them and a perturbation moves them
 * together.
 */
/* Frame offset of the EtherType, after the two 6-byte MAC addresses. libreac
 * and reac-pw tools open-code `frame[12] == 0x88 && frame[13] == 0x19` rather
 * than naming the position. [EVIDENCED (corpus) — every frame in 72 captures;
 * reac.ksy seq/0..2 (6 + 6, then the contents).]
 */
#define REAC_ETHERTYPE_OFF              12

/* A MAC address — the frame's destination and source, the scene's master_id
 * and peer ids, and the config announce's master MAC. [EVIDENCED (corpus).]
 */
#define REAC_ETH_ADDR_BYTES             6

/* Width of the little-endian frame counter at HDR_COUNTER_OFF. [EVIDENCED
 * (corpus).]
 */
#define REAC_HDR_COUNTER_BYTES          2

/* Width of the big-endian type word at TYPED_BLOCK_OFF, the 2 in
 * TYPED_BLOCK_LEN = 2 + 32 and in every two-byte `pos` of typed_block.
 * [EVIDENCED (corpus).]
 */
#define REAC_TYPE_WORD_BYTES            2

/* The two end-marker bytes, END_MARKER_0 and END_MARKER_1 — the 2 in
 * FRAME_OVERHEAD = 50 + 2. [EVIDENCED (corpus).]
 */
#define REAC_END_MARKER_BYTES           2

/* The braid carries channels in PAIRS — one 6-byte pair group per two
 * channels per sample time — so every legal audio width is even. The
 * byte permutation inside a pair stays libreac's (see the header of
 * this file); the pairing itself is spelled by reac.ksy's
 * `num_channels / 2` and by every even-width check in libreac and
 * reac-pw, so it is shared.
 *   [EVIDENCED (corpus) — reac.ksy time_sample, validated against libreac's
 *   upstream goldens by reac_xcheck.py.]
 */
#define REAC_BRAID_PAIR_CHANNELS        2

/* Neither segment bit — a frame in the middle of a bulk transfer. [EVIDENCED
 * (image) — FUN_0c003398 writes block[1] = 0 for a middle frame
 * (control_header group doc).]
 */
#define REAC_SEG_MIDDLE                 0x00

/* block[0] of a master announce (type word 0xcfea, op 0xffff). [EVIDENCED
 * (corpus).]
 */
#define REAC_LINK_ANNOUNCE              0xff

/* ---- Pace — packet rates, sample rates and the pace code -----------------------
 * The master sets the pace (PACE_IS_THE_MASTERS) and there are exactly
 * three. A packet carries SAMPLES_PER_PKT samples per channel, so the
 * packet rate is the quantity on the wire and the sample rate is derived
 * from it. The PACE CODE is the one byte by which the master declares and
 * records the class, on four carriers (CONSOLE_FIELD_GATES_RATE):
 * ANNOUNCE_PACE_OFF, SCENE_REVISION_OFF, the chanmap marker's flags and
 * ENROLL_PACE_OFF.
 *
 * libreac (reac.c, reac_master.c, reac_cfg.h), reac-pw (reac_rate_cfg.c,
 * main.c, reac_sink_node.c) and a dozen tools spelled 44100/48000/96000
 * and 3675/4000/8000 and 0/1/2 themselves; this is the first place they
 * are declared.
 */
/* Frames per second at 48 kHz. [EVIDENCED (corpus) — pcap timestamps, every
 * 48 kHz capture.]
 */
#define REAC_PKT_RATE_48K               4000

/* Frames per second at 96 kHz. [EVIDENCED (corpus + rig) — 96 kHz windows
 * measured at 8000 fps (CONSOLE_FIELD_GATES_RATE evidence).]
 */
#define REAC_PKT_RATE_96K               8000

/* Frames per second at 44.1 kHz. [EVIDENCED (corpus) —
 * m200-enrol-441k-2026-09-13 and m200-enrol-s4000-441k-2026-09-13, 3675 fps
 * from the pcap timestamps.]
 */
#define REAC_PKT_RATE_44K1              3675

/* The 48 kHz class. [derived — 4000 frames x 12 samples.] */
#define REAC_SAMPLE_RATE_48K            48000

/* The 96 kHz class. [derived — 8000 frames x 12 samples.] */
#define REAC_SAMPLE_RATE_96K            96000

/* The 44.1 kHz class. [derived — 3675 frames x 12 samples.] */
#define REAC_SAMPLE_RATE_44K1           44100

/* ---- The pace code -------------------------------------------------------------
 * The byte the four rate carriers hold (see the pace group).
 */
/* 48 kHz. Was called the V-Mixer family. [RIG-VERIFIED (2026-08-27) and
 * WIRE-CAPTURED (2026-09-13) — see CONSOLE_FIELD_GATES_RATE.]
 */
#define REAC_PACE_CODE_48K              0x00

/* 96 kHz. Was called the OHRCA family. [RIG-VERIFIED (2026-08-27) — see
 * CONSOLE_FIELD_GATES_RATE.]
 */
#define REAC_PACE_CODE_96K              0x01

/* 44.1 kHz. [WIRE-CAPTURED (2026-09-13) — an M-200 at 44.1 kHz writes 0x02 on
 * all four carriers. RULED (operator, 2026-09-13).]
 */
#define REAC_PACE_CODE_44K1             0x02

/* ---- The filler descriptor -----------------------------------------------------
 * An upstream FILLER's 32-byte control area is not always zero: a box
 * stamps `00 <desc>` sixteen times across it, and a real S-1608 REFUSES an
 * enrolment whose filler window is zeroed. libreac's reac_link.h,
 * reac_ctrlblk.c, reac_ctrl.c, reac_master.c and transport/reac_slave.c
 * and reac-pw's pcap-variants.py each spelled the three values.
 */
/* Before the box has asked — the first frames after link-up. [EVIDENCED
 * (corpus) — the granted S-0808 sent 48 zero frames first (wire,
 * 2026-09-09).]
 */
#define REAC_FILLER_DESC_NONE           0x00

/* Announce sent, grant not yet received. [EVIDENCED (corpus + replay) — 8691
 * frames between the S-0808's announce and its grant (wire, 2026-09-09;
 * box-to-box-enroll.pcap).]
 */
#define REAC_FILLER_DESC_REQUESTING     0x52

/* Granted. Claiming it before the grant gets the enrolment refused.
 * [EVIDENCED (corpus + replay) — from t=7.619 in box-to-box-enroll.pcap.]
 */
#define REAC_FILLER_DESC_ESTABLISHED    0x7a

/* ---- The Roland DT1 record inside op-0403 --------------------------------------
 * The link-4 container's record, byte for byte. reac.ksy spells it as
 * `dt1_record` contents; libreac's reac_ctrlblk.c and reac_box_synth.c
 * wrote the same bytes and block offsets as literals (`out[9] = 0xf0`,
 * `blk[15] == 0x12`), and reac-pw's pcap-variants.py a third time. Block
 * offsets are block-relative, like every offset in this file.
 */
/* The four bytes at block[4:8] ahead of every DT1 record and record fragment.
 * Its tail `02 00 fe` is the box-return marker's too, which is why
 * SUB_0403_OFF alone discriminates them. [EVIDENCED (corpus).]
 */
#define REAC_DT1_WRAPPER                0x000200fe

/* Width of DT1_WRAPPER. [EVIDENCED (corpus).] */
#define REAC_DT1_WRAPPER_BYTES          4

/* Block offset of the SysEx length echo, right after the wrapper. [EVIDENCED
 * (corpus).]
 */
#define REAC_DT1_LEN_ECHO_OFF           8

/* Block offset of SYSEX_START. [EVIDENCED (corpus).] */
#define REAC_DT1_SYSEX_OFF              9

/* MIDI SysEx start. [EVIDENCED (corpus).] */
#define REAC_SYSEX_START                0xf0

/* Roland's SysEx manufacturer id. [EVIDENCED (corpus).] */
#define REAC_ROLAND_ID                  0x41

/* The device id byte every captured box-built record carries. [EVIDENCED
 * (corpus) — the cold-connect records of matrix-m200-s1608 and
 * matrix-m200-s0808 (2026-07-11). reac.ksy parses it as a free u1 and does
 * not validate it.]
 */
#define REAC_DT1_DEVICE_ID              0x0a

/* SYSEX_START, ROLAND_ID and the device id — the bytes before the model id.
 * [EVIDENCED (corpus).]
 */
#define REAC_DT1_SYSEX_HEAD_BYTES       3

/* The Roland model id, `00 00 12`. [EVIDENCED (corpus).] */
#define REAC_DT1_MODEL_ID_BYTES         3

/* The model id's low byte, which the classifier checks to tell a genuine DT1
 * record from the look-alikes. [EVIDENCED (corpus).]
 */
#define REAC_DT1_MODEL_ID_LO            0x12

/* Block offset of DT1_MODEL_ID_LO. [EVIDENCED (corpus).] */
#define REAC_DT1_MODEL_LO_OFF           14

/* Block offset of the command byte, DT_CMD_RQ1 or DT_CMD_DT1. [EVIDENCED
 * (corpus).]
 */
#define REAC_DT1_CMD_OFF                15

/* Block offset of the big-endian register-page tag (the dt1_tag group).
 * [EVIDENCED (corpus).]
 */
#define REAC_DT1_TAG_OFF                16

/* Roland RQ1 — a read request (the identity poll). [EVIDENCED (corpus).] */
#define REAC_DT_CMD_RQ1                 0x11

/* Roland DT1 — data set. [EVIDENCED (corpus).] */
#define REAC_DT_CMD_DT1                 0x12

/* MIDI SysEx end, after the inner checksum. [EVIDENCED (corpus).] */
#define REAC_SYSEX_END                  0xf7

/* rec_len minus the SysEx record's own length. [EVIDENCED (corpus).] */
#define REAC_DT1_RECORD_OVERHEAD        0x0d

/* rec_len minus the record's data bytes. [EVIDENCED (corpus).] */
#define REAC_DT1_DATA_OVERHEAD          0x10

/* ---- Identity page fields reac.ksy parses and libreac builds -------------------
 * The rest of the identity_addr enum reac.ksy dispatches on, and the reply
 * sizes its `if:` guards test, which libreac's reac_identity.c and
 * reac_box_synth.c and reac-pw's test DT1 builders spelled as literals.
 */
/* The model-name continuation address. [EVIDENCED (corpus).] */
#define REAC_IDENTITY_ADDR_MODEL_NAME_EXT 0x1011

/* The second model-name slot. [EVIDENCED (corpus).] */
#define REAC_IDENTITY_ADDR_MODEL_NAME_SLOT_B 0x1100

/* The second slot's continuation. [EVIDENCED (corpus).] */
#define REAC_IDENTITY_ADDR_MODEL_NAME_SLOT_B_EXT 0x1111

/* The firmware version reply — four bytes, one decimal digit each (2200 is
 * `02 02 00 00`). [EVIDENCED (corpus).]
 */
#define REAC_IDENTITY_FIRMWARE_BYTES    4

/* The REAC version reply — four big-endian u16s, reserved / major / minor /
 * patch. [EVIDENCED (corpus).]
 */
#define REAC_IDENTITY_REAC_VERSION_BYTES 8

/* ---- The config announce and the enroll group map, field by field --------------
 * Block offsets of the fields reac.ksy's `cfea_payload` and
 * `enroll_group_map` parse by sequence and libreac's reac_master.c /
 * tools/group_map_scan.c index by literal. The pace code sits at
 * ANNOUNCE_PACE_OFF (the "cfea[19]" of the prose above, which counts from
 * the type word) and at ENROLL_PACE_OFF ("ENROLL[8]").
 */
/* The fixed `01 03 0d 01 04` after the op word. [EVIDENCED (corpus) — 17,040
 * announces, 2026-09-13.]
 */
#define REAC_ANNOUNCE_HEAD_BYTES        5

/* The master's MAC. [EVIDENCED (corpus).] */
#define REAC_ANNOUNCE_MAC_OFF           9

/* The fabric's slot total (0x28 on every capture). [EVIDENCED (corpus).] */
#define REAC_ANNOUNCE_TOTAL_SLOTS_OFF   15

/* The announced box input width. [EVIDENCED (corpus).] */
#define REAC_ANNOUNCE_BOX_IN_WIDTH_OFF  16

/* console_field — the pace code. [EVIDENCED (corpus + rig) — see
 * CONSOLE_FIELD_GATES_RATE.]
 */
#define REAC_ANNOUNCE_PACE_OFF          17

/* The big-endian box count. [EVIDENCED (corpus).] */
#define REAC_ANNOUNCE_BOX_COUNT_OFF     18

/* Block offset of the commit report's board-configuration code — the chassis
 * strap HEADAMP_BASE_FROM_CONFIG_BYTE7 names, which libreac (reac_ports.h,
 * reac_box_synth.c) and its tests index as a bare 7. After the subtype and
 * two zero bytes, right before the inventory. [EVIDENCED (image + corpus) —
 * S-1608 FUN_0c003c8a; see LEN_SUB_COMMIT_REPORT.]
 */
#define REAC_BOARD_CONFIG_OFF           7

/* The enroll group map's console byte — the pace code. [EVIDENCED (corpus) —
 * m200-enrol-441k-2026-09-13.]
 */
#define REAC_ENROLL_PACE_OFF            6

/* First of ENROLL_GROUPS input-group cells. [EVIDENCED (image + corpus).] */
#define REAC_ENROLL_IN_GROUPS_OFF       7

/* First of ENROLL_GROUPS output-group cells. Five in plus five out is the
 * ten-cell run tools/group_map_scan.c scans. [EVIDENCED (image + corpus).]
 */
#define REAC_ENROLL_OUT_GROUPS_OFF      12

/* Channels per enroll group — ENROLL_GROUP_IN counts eight inputs. [EVIDENCED
 * (image + corpus).]
 */
#define REAC_ENROLL_GROUP_CHANNELS      8

/* ---- The scene body's tag words ------------------------------------------------
 * Only their OFFSETS were declared (scene_body group). The four ASCII bytes
 * at each were spelled again by reac.ksy's contents, libreac's
 * reac_ctrlblk.c / reac_master.c and reac-pw's recover-scene.py, as
 * strings. Declared as big-endian u32 so every target can carry them.
 */
/* "1234" at SCENE_TAG_ID_OFF. [EVIDENCED (corpus) — 27 real-desk bodies.] */
#define REAC_SCENE_TAG_ID               0x31323334

/* "SYSP" at SCENE_TAG_SYSP_OFF. [EVIDENCED (corpus).] */
#define REAC_SCENE_TAG_SYSP             0x53595350

/* "SCEN" at SCENE_TAG_SCEN_OFF. [EVIDENCED (corpus).] */
#define REAC_SCENE_TAG_SCEN             0x5343454e

/* The little-endian `revision` at SCENE_REVISION_OFF — the pace code's scene
 * carrier. [EVIDENCED (corpus + rig).]
 */
#define REAC_SCENE_REVISION_BYTES       2

/* ---- What each box model declares ----------------------------------------------
 * A box's geometry is READ FROM ITS DECLARATION (the inventory group) and
 * nothing may key behaviour on a model. But emulators, fixtures and tests
 * in libreac (reac_ctrlblk.c box table, transport/reac_slave.h) and
 * reac-pw spell what each model declares, and those counts are protocol
 * observations, so they are declared once here rather than per consumer.
 */
/* S-0808 analog inputs. [EVIDENCED (corpus) — config-announce inventory,
 * fixtures/control.json; 24-record head-amp sweeps.]
 */
#define REAC_BOX_S0808_IN               8

/* S-0808 outputs. [EVIDENCED (corpus) — config-announce inventory.] */
#define REAC_BOX_S0808_OUT              8

/* S-1608 analog inputs. [EVIDENCED (corpus) — inventory; 48-record sweeps.] */
#define REAC_BOX_S1608_IN               16

/* S-1608 outputs. [EVIDENCED (corpus) — config-announce inventory.] */
#define REAC_BOX_S1608_OUT              8

/* S-4000S in its 32-in/8-out configuration. [EVIDENCED (corpus) — inventory;
 * 96-record sweeps.]
 */
#define REAC_BOX_S4000S_3208_IN         32

/* S-4000S 32x8 outputs. [EVIDENCED (corpus).] */
#define REAC_BOX_S4000S_3208_OUT        8

/* S-4000S in its 8-in/32-out configuration. [EVIDENCED (corpus) —
 * vlan13-0832.pcap.]
 */
#define REAC_BOX_S4000S_0832_IN         8

/* S-4000S 8x32 outputs. [EVIDENCED (corpus) — vlan13-0832.pcap.] */
#define REAC_BOX_S4000S_0832_OUT        32

/* ---- Timing a second implementation has to match -------------------------------
 * Periods and budgets a box or a desk FIXES and a peer must honour, as
 * opposed to this project's own tunables (which live in libreac's
 * reac_tunables.h and are not protocol). Each was a literal in libreac's
 * reac_fsm.h / reac_master.h / reac_master.c.
 */
/* The box firmware's established link-check reload, in frames. [EVIDENCED
 * (image) — 0x0258 in the S-1608 image.]
 */
#define REAC_BOX_LINKCHECK_RELOAD_FRAMES 600   /* 0x0258 */

/* A master announces, and while established sends one chanmap window, once a
 * second. [EVIDENCED (corpus + rig) — at the hunt rate (~0.37/s) the box's
 * link light kept blinking (rig, 2026-07-12).]
 */
#define REAC_ANNOUNCE_PERIOD_MS         1000

/* A desk's scene push rate — 341 chunks in ~0.68 s. [EVIDENCED (corpus) —
 * M-200i -> S-1608, SCENE_CHUNKS evidence.]
 */
#define REAC_SCENE_BURST_CHUNKS_PER_SEC 500

/* One echoed grant per twelve frame slots across the ~150 ms grant burst.
 * [EVIDENCED (corpus) — the transcribed real burst.]
 */
#define REAC_GRANT_STRIDE_SLOTS         12

/* A desk's dwell between the enroll group map and the grant burst. [EVIDENCED
 * (corpus) — 1503 ms on matrix-m200-s0808 and 1717 ms on matrix-m200-s1608
 * (2026-07-11); nominal.]
 */
#define REAC_ENROLL_GRANT_DWELL_MS      1600

/* How long a desk rides through box silence before it reverts to hunting.
 * [EVIDENCED (rig) — one M-200i reboot measurement, 2026-07-11 (heartbeat
 * stops t=16.0 s, first probe t=22.47 s).]
 */
#define REAC_MASTER_LINK_HOLD_MS        6500

#endif /* REAC_FACTS_H */
