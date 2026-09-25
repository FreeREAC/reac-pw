/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * reac_facts_pw.h — THE ONE DOOR TO THE REAC PROTOCOL'S NUMBERS.
 *
 * Every protocol number reac-pw uses is read from reac-protocol's declaration
 * (spec/protocol-facts.yaml -> spec/gen-facts.py -> reac_facts.h), never spelled
 * here. The operator's ruling of 2026-09-25: "declare only once and use it
 * everywhere — a single source of truth; hardcoded constants are the worst
 * practice and a source of nasty bugs". A fact that is not declared there is not
 * declared here either: reac-protocol owns declarations.
 *
 * WHERE THE HEADER COMES FROM. facts/reac_facts.h is a verbatim copy of the
 * generated header, held to reac-protocol by the `facts_drift` test
 * (tools/facts.py drift). The meson option `facts_dir` points the build at another
 * copy; tools/facts-perturb-check.sh uses it to build against a PERTURBED set,
 * where every free number moves, so a literal left in reac-pw disagrees with the
 * header and a test goes red.
 *
 * WHY THIS ORDER. libreac's public headers still define about fifty of the same
 * names by hand, some spelled differently (0xC2 / 0xc2, 0x0600u / 0x0600). They
 * are included first, then reac_facts_undef.h clears every fact name (it is derived
 * from reac_facts.h by name, by the same tool), and reac_facts.h defines them last.
 * So reac-pw compiles against the generated value, with no redefinition diagnosed,
 * and a later #include of those libreac headers is a no-op behind their guards. */
#ifndef REACPW_FACTS_PW_H
#define REACPW_FACTS_PW_H

#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_identity.h>
#include <reac/reac_ports.h>

#include "reac_facts_undef.h"
#include "reac_facts.h"

/* A FACT INSIDE A STRING. A help text or a log line that states a protocol number
 * spells it through this, so the text is the generated value too:
 *   "rates: " REACPW_STR(REAC_SAMPLE_RATE_48K)  ->  "rates: 48000"
 * (two levels, so the macro's VALUE is stringified and not its name). */
#define REACPW_STR_(x) #x
#define REACPW_STR(x)  REACPW_STR_(x)

/* A frame's length at n channels: the protocol's one geometry law, 52 + n*36, from its
 * facts (REAC_FRAME_OVERHEAD, REAC_BYTES_PER_CHANNEL). */
#define REACPW_FRAME_LEN(n) (REAC_FRAME_OVERHEAD + (n) * REAC_BYTES_PER_CHANNEL)

/* A sample's width in bits (24 today): its sign bit, its mask and its full scale follow. */
#define REACPW_SAMPLE_BITS  (8 * REAC_RESOLUTION)
#define REACPW_SAMPLE_SIGN  (1LL << (REACPW_SAMPLE_BITS - 1))
#define REACPW_SAMPLE_MASK  ((1LL << REACPW_SAMPLE_BITS) - 1)

/* Head-amp channel space: the highest wire channel, and the S-1608's base —
 * its strap (2; the per-model strap is not declared in reac-protocol) times the
 * declared multiplier. */
#define REACPW_HEADAMP_TOP_CH      (REAC_HEADAMP_CH_SPAN - 1)
#define REACPW_S1608_HEADAMP_BASE  (2 * REAC_HEADAMP_BASE_MULTIPLIER)
/* A w-wide box's grant sweep: 2 head frames + 6 group-B records (neither count
 * is declared in reac-protocol yet) + one group-A record per input per param. */
#define REACPW_GRANT_SWEEP_LEN(w)  (8 + (w) * REAC_HEADAMP_SWEEP_RECORDS_PER_CH)
/* The declared sensitivity rule, in centi-dB: step 0 is the reference, each
 * step one STEP_CDB lower, the pad adds PAD_CDB. */
#define REACPW_SENS_CDB(step, pad) (REAC_HEADAMP_SENS_REF_CDB - (step) * REAC_HEADAMP_SENS_STEP_CDB \
                                    + ((pad) ? REAC_HEADAMP_PAD_CDB : 0))

/* Where a scene-body offset lands on the wire: which chunk (0-based, step - 1)
 * and how far into that chunk's payload. */
#define REACPW_SCENE_CHUNK_OF(off) (((off) - REAC_SCENE_HEAD_BYTES) / REAC_SCENE_CHUNK_BYTES)
#define REACPW_SCENE_IN_CHUNK(off) (((off) - REAC_SCENE_HEAD_BYTES) % REAC_SCENE_CHUNK_BYTES)

/* A big-endian u16 at p: the type word, the opcode (link, segment) and the length. */
#define REACPW_BE16(p) ((unsigned)((const uint8_t *)(p))[0] << 8 | ((const uint8_t *)(p))[1])

/* The legal paces, as text: "44100, 48000 or 96000" and "44100|48000|96000". */
#define REACPW_RATES_OR   REACPW_STR(REAC_SAMPLE_RATE_44K1) ", " \
                          REACPW_STR(REAC_SAMPLE_RATE_48K) " or " \
                          REACPW_STR(REAC_SAMPLE_RATE_96K)
#define REACPW_RATES_BAR  REACPW_STR(REAC_SAMPLE_RATE_44K1) "|" \
                          REACPW_STR(REAC_SAMPLE_RATE_48K) "|" \
                          REACPW_STR(REAC_SAMPLE_RATE_96K)

/* A control-block offset seen from the frame, and from the typed window (the
 * type word first) — the two indexings tests and sweeps use. */
#define REACPW_FRAME_OF(blk_off) (REAC_CTRL_BLOCK_OFF + (blk_off))
#define REACPW_TYPED_OF(blk_off) (REAC_TYPE_WORD_BYTES + (blk_off))

/* Inside a DT1 record: the 4-byte address is the page tag then addr_lo, and the
 * data follows it. On the head-amp page addr_lo is (CH, PARAM), then one VALUE
 * byte and the inner checksum. */
#define REACPW_DT1_ADDR_OFF  (REAC_DT1_TAG_OFF + REAC_IDENTITY_ADDR_BYTES - REAC_IDENTITY_ADDR_LO_BYTES)
#define REACPW_DT1_DATA_OFF  (REAC_DT1_TAG_OFF + REAC_IDENTITY_ADDR_BYTES)
#define REACPW_HA_CH_OFF     REACPW_DT1_ADDR_OFF
#define REACPW_HA_PARAM_OFF  (REACPW_DT1_ADDR_OFF + 1)
#define REACPW_HA_VALUE_OFF  REACPW_DT1_DATA_OFF
#define REACPW_HA_CKSUM_OFF  (REACPW_DT1_DATA_OFF + 1)
#define REACPW_HA_REC_LEN    (REACPW_HA_CKSUM_OFF + 1 - REAC_DT1_TAG_OFF)   /* TAG..CKSUM */

/* One Roland DT1 record, laid into the control block at blk the way reac.ksy's
 * dt1_record spells it: link 4 single, rec_len, the wrapper, the SysEx length
 * echo, F0 41 <dev> <model> 12, the page tag, addr_lo, n payload bytes, a
 * stand-in inner checksum and F7. Every offset and byte is a declared fact;
 * callers own the rest of the frame. */
static inline void reacpw_dt1_record(uint8_t *blk, unsigned tag, unsigned addr_lo,
                                     const uint8_t *pl, size_t n)
{
	const unsigned data = REAC_IDENTITY_ADDR_LO_BYTES + (unsigned)n;
	const unsigned rec_len = REAC_DT1_DATA_OVERHEAD + data;
	const unsigned a = REACPW_DT1_ADDR_OFF;
	const unsigned d = REACPW_DT1_DATA_OFF;
	blk[REAC_HDR_LINK_OFF] = REAC_LINK_RECORD;
	blk[REAC_HDR_SEG_OFF] = REAC_SEG_SINGLE;
	blk[REAC_HDR_LEN_OFF] = (uint8_t)(rec_len >> 8);
	blk[REAC_HDR_LEN_OFF + 1] = (uint8_t)rec_len;
	for (int i = 0; i < REAC_DT1_WRAPPER_BYTES; i++)
		blk[REAC_HDR_OPCODE_OFF + i] =
			(uint8_t)(REAC_DT1_WRAPPER >> (8 * (REAC_DT1_WRAPPER_BYTES - 1 - i)));
	blk[REAC_DT1_LEN_ECHO_OFF] = (uint8_t)(rec_len - (REAC_DT1_SYSEX_OFF - REAC_HDR_OPCODE_OFF));
	blk[REAC_DT1_SYSEX_OFF] = REAC_SYSEX_START;
	blk[REAC_DT1_SYSEX_OFF + 1] = REAC_ROLAND_ID;
	blk[REAC_DT1_SYSEX_OFF + 2] = REAC_DT1_DEVICE_ID;
	blk[REAC_DT1_MODEL_LO_OFF] = REAC_DT1_MODEL_ID_LO;
	blk[REAC_DT1_CMD_OFF] = REAC_DT_CMD_DT1;
	blk[REAC_DT1_TAG_OFF] = (uint8_t)(tag >> 8);
	blk[REAC_DT1_TAG_OFF + 1] = (uint8_t)tag;
	blk[a] = (uint8_t)(addr_lo >> 8);
	blk[a + 1] = (uint8_t)addr_lo;
	for (size_t i = 0; i < n; i++)
		blk[d + i] = pl[i];
	blk[d + n] = 0x7f;                       /* stand-in; the parser reads structure */
	blk[d + n + 1] = REAC_SYSEX_END;
}

#endif /* REACPW_FACTS_PW_H */
