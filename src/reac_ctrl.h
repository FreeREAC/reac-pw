// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_ctrl — REAC control-plane: the cdea/cfea checksum, a classifying parser,
 * and the slave/stagebox-side frame builders (heartbeat, config-announce,
 * cold-connect, upstream return FILLER). The byte layouts are ground-truthed
 * against the real bidirectional captures (reac-captures/, wired-reac-*-bothdirs);
 * the JOIN frames we EMIT (cold-connect, config-announce) are reconstructed from
 * the §13b/§13d transcription + firmware and are flagged experimental until a
 * fresh PHY-link-up rig capture confirms the master's grant-burst.
 *
 * See reac-firmware-re/REAC-CONNECTION-FSM.md for the full spec + evidence grades.
 *
 * Direction discipline: the master BROADCASTS downstream; we (a virtual stagebox)
 * UNICAST upstream to the learned master MAC with our Roland-OUI src. Control
 * block is the 32 bytes [18:50]; cdea/cfea carry a checksum at [49] such that
 * Sum(frame[18..49]) mod 256 == 0. FILLER (type 00 00) is checksum-exempt. */
#ifndef REAC_CTRL_H
#define REAC_CTRL_H

#include <stdint.h>
#include <stddef.h>

#define REAC_CTRL_BLOCK_OFF   18   /* control block / checksum region start */
#define REAC_CTRL_BLOCK_END   50   /* one past end (= audio offset)         */
#define REAC_CTRL_CKSUM_OFF    49  /* checksum byte (last of the block)     */

enum reac_ctrl_kind {
	REAC_CTRL_NONE = 0,      /* not a 0x8819 frame */
	REAC_CTRL_FILLER,        /* type 00 00 (audio/idle), checksum-exempt */
	REAC_CTRL_PROBE,         /* master cdea 01, sub-state cycling (hunting) */
	REAC_CTRL_MASTER_HB,     /* master cdea 01 03 0019 (established heartbeat) */
	REAC_CTRL_MASTER_ANNOUNCE,/* master cfea (announce) */
	REAC_CTRL_GRANT,         /* master cdea 04 03 (the JOIN grant-burst) */
	REAC_CTRL_BOX_HB,        /* a box cdea 01 03 0001 81 (our keep-alive) */
	REAC_CTRL_UNKNOWN_CTRL,  /* cdea/cfea we don't classify */
};

struct reac_ctrl_parsed {
	enum reac_ctrl_kind kind;
	uint8_t  src[6];
	uint8_t  dst[6];
	int      is_broadcast;   /* dst == ff:ff:ff:ff:ff:ff */
	uint16_t counter;        /* bytes 14-15 LE */
	uint8_t  op0, op1;       /* control opcode bytes [18],[19] */
	uint16_t op_len;         /* BE length [20:22] */
	uint8_t  sel;            /* selector [22] (0x81/0x82/... or a channel byte) */
};

/* Checksum over the 32-byte control block [18:50]: set frame[49] so the block
 * sums to 0 mod 256. Verify returns 0 when Sum(frame[18..49]) mod 256 == 0. */
void reac_ctrl_checksum_apply(uint8_t *frame);
int  reac_ctrl_checksum_verify(const uint8_t *frame);

/* Classify a raw ethernet frame; fills *out. Returns out->kind. master_mac is
 * the ethernet SOURCE for any master frame — callers learn/pin it from
 * out->src when the frame came from the master (broadcast or unicast-to-us). */
enum reac_ctrl_kind reac_ctrl_parse(const uint8_t *frame, size_t len,
                                    struct reac_ctrl_parsed *out);

/* Builders. All emit unicast-to-master (dst=master), our Roland-OUI src, the
 * given free-running u16-LE counter, EtherType 0x8819, checksum applied.
 * Return the frame length in bytes, or 0 on bad args.
 *
 * GROUND-TRUTHED (byte-compared to captured box frames): */
size_t reac_ctrl_build_box_hb(uint8_t *out, const uint8_t master[6],
                              const uint8_t src[6], uint16_t counter);
/* upstream return audio: n_ch x 12 samples, planar float [ch][s]; box-width
 * frame (16ch->628B, 8ch->340B): 18 hdr + 32 descriptor + n_ch*36 audio + 2 tail. */
size_t reac_ctrl_build_upstream_filler(uint8_t *out, const uint8_t master[6],
                                       const uint8_t src[6], uint16_t counter,
                                       int n_ch, float *const *planar, int ns);

/* RECONSTRUCTED (experimental, JOIN — not byte-verified, gated until a rig grab): */
size_t reac_ctrl_build_config_announce(uint8_t *out, const uint8_t master[6],
                                       const uint8_t src[6], uint16_t counter, int in_ch);
size_t reac_ctrl_build_coldconnect(uint8_t *out, const uint8_t master[6],
                                   const uint8_t src[6], uint16_t counter);

#endif /* REAC_CTRL_H */
