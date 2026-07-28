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

#include "reac_master.h"   /* enum reac_master_rx_event (the classifier's verdict) */

#define REAC_CTRL_BLOCK_OFF   18   /* control block / checksum region start */
#define REAC_CTRL_BLOCK_END   50   /* one past end (= audio offset)         */
#define REAC_CTRL_CKSUM_OFF    49  /* checksum byte (last of the block)     */
#define REAC_CTRL_BLOCK_LEN   32   /* the checksummed block, [18:50]        */

enum reac_ctrl_kind {
	REAC_CTRL_NONE = 0,      /* not a 0x8819 frame */
	REAC_CTRL_FILLER,        /* type 00 00 (audio/idle), checksum-exempt */
	REAC_CTRL_PROBE,         /* master cdea 01, sub-state cycling (hunting) */
	REAC_CTRL_MASTER_HB,     /* master cdea 01 03 0019 (established heartbeat) */
	REAC_CTRL_MASTER_ANNOUNCE,/* master cfea (announce) */
	REAC_CTRL_GRANT,         /* master cdea 04 03, record TAG 01 00 (the JOIN
	                          * grant-burst; also any 04 03 tag we don't know) */
	REAC_CTRL_HEADAMP,       /* master cdea 04 03, record TAG 01 01 (head-amp:
	                          * CH PARAM VALUE — a preamp knob, NOT a grant) */
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
	uint8_t  sel2;           /* second selector byte [23] (cold-connect: 0x02) */
	uint8_t  ch;             /* HEADAMP only: wire channel (model_base + input-1) */
	uint8_t  param;          /* HEADAMP only: enum reac_headamp_param */
	uint8_t  value;          /* HEADAMP only: 0|1 (phantom/pad) or 0x00..0x37 (SENS) */
};

/* Checksum over the 32-byte control block [18:50]: set frame[49] so the block
 * sums to 0 mod 256. Verify returns 0 when Sum(frame[18..49]) mod 256 == 0. */
void reac_ctrl_checksum_apply(uint8_t *frame);
int  reac_ctrl_checksum_verify(const uint8_t *frame);

/* The underlying sum-to-ZERO rule, on a bare 32-byte block (no frame offsets):
 * set block[31] so Sum(block[0..31]) mod 256 == 0. This is THE cdea/cfea
 * control-block checksum — reac_ctrl_checksum_apply() is this on
 * frame + REAC_CTRL_BLOCK_OFF, and reac_master's 34-byte control templates
 * (type word + block) apply it at template + 2. One implementation; the two
 * offset bases were previously maintained as independent loops. */
void reac_ctrl_block_cksum_stamp(uint8_t block[REAC_CTRL_BLOCK_LEN]);

/* The INNER record rule, sum-to-0x80: a DT1-style record (TAG.. payload..
 * CKSUM, e.g. the 6-byte head-amp record at frame[34:40]) carries its last
 * byte such that the whole record sums to 0x80 mod 256 (byte-verified on the
 * M-200, m200-headamp-re/DECODE.md). stamp sets rec[n-1]; verify returns 0
 * when Sum(rec[0..n-1]) mod 256 == 0x80. The rule is the record's, not the
 * head-amp's — any future TAG reuses these. */
void reac_ctrl_record_cksum_stamp(uint8_t *rec, size_t n);
int  reac_ctrl_record_cksum_verify(const uint8_t *rec, size_t n);

/* Classify a raw ethernet frame; fills *out. Returns out->kind. master_mac is
 * the ethernet SOURCE for any master frame — callers learn/pin it from
 * out->src when the frame came from the master (broadcast or unicast-to-us). */
enum reac_ctrl_kind reac_ctrl_parse(const uint8_t *frame, size_t len,
                                    struct reac_ctrl_parsed *out);

/* MASTER-side box-frame classifier (PURE — no socket): decide whether a raw
 * received frame is a box frame the master FSM cares about, and which
 * reac_master_rx_event it is. Returns 0 with out + ev filled, or -1 for
 * anything else (not 0x8819 / not Roland OUI / our own echo / another
 * master's broadcast). Matcher rules (byte-verified zoneA-48k JOIN):
 *   - broadcast type-0000 -> BCAST_FILLER (the presence-flood; diagnostic);
 *   - cdea 04 03, BE len 0x0013/0x0014, then 00 02, checksum valid -> JOIN,
 *     accepted broadcast OR unicast (the box emits it x3 on PHY-up while
 *     still in broadcast mode). Keyed ONLY on block[0:6] + checksum — the
 *     tail (0x41 ...) is device inventory, never matched;
 *   - unicast-to-us box heartbeat (cdea 01 03 0001) sel 0x81 -> UNICAST,
 *     sel 0x00 -> BYE (the explicit disconnect);
 *   - any other unicast-to-us box frame (upstream FILLER 628/340 B,
 *     config-announce sel 0x82, unknown ctrl) -> UNICAST.
 * reac_ctrl_parse's GRANT kind is direction-blind (a box cold-connect and a
 * master grant are the same cdea 04 03 bytes) — this classifier disambiguates
 * by src, not by adding new kinds. */
int reac_ctrl_classify_box_frame(const uint8_t *frame, size_t len,
                                 const uint8_t our_mac[6],
                                 struct reac_ctrl_parsed *out,
                                 enum reac_master_rx_event *ev);

/* Builders. All emit unicast-to-master (dst=master), our Roland-OUI src, the
 * given free-running u16-LE counter, EtherType 0x8819, checksum applied.
 * Return the frame length in bytes, or 0 on bad args.
 *
 * GROUND-TRUTHED (byte-compared to captured box frames): */
size_t reac_ctrl_build_box_hb(uint8_t *out, const uint8_t master[6],
                              const uint8_t src[6], uint16_t counter, int n_ch);
/* upstream return audio: n_ch x 12 samples, planar float [ch][s]; box-width
 * frame (16ch->628B, 8ch->340B): 18 hdr + 32 descriptor + n_ch*36 audio + 2 tail. */
size_t reac_ctrl_build_upstream_filler(uint8_t *out, const uint8_t master[6],
                                       const uint8_t src[6], uint16_t counter,
                                       int n_ch, float *const *planar, int ns);

/* The presence-flood FILLER (broadcast, unlinked): zero control block [18:50] (no
 * 0x7a descriptor) over LIVE audio [50:626] — what a real box broadcast-floods to
 * announce presence on a cold boot. Audio is planar float [ch][s], as
 * build_upstream_filler; NULL planar -> silent. */
size_t reac_ctrl_build_flood_filler(uint8_t *out, const uint8_t bcast[6],
                                    const uint8_t src[6], uint16_t counter,
                                    int n_ch, float *const *planar, int ns);

/* ---- FIXED box-model matrix ----
 * A REAC stagebox is identified on the wire by three orthogonal fields (see
 * docs/REAC-BOX-STATE-DIAGRAM.md): the config-announce SELECTOR byte (model
 * family), an optional ASCII NAME frame (exact model within the 0x84 family),
 * and the channel DESCRIPTOR + width (52 + 36*in_ch bytes). We ship a fixed
 * table of byte-verified real models so a model always matches its channels —
 * there is no "S-1608 with 8 channels". Pick a row by token or by in-channel
 * count; both resolve to the same entry. */
struct reac_box_model {
	const char *token;      /* CLI token: "s1608", "s0808"              */
	const char *display;    /* human label for --help / logs            */
	int         in_ch;      /* box input (upstream) width -> frame size  */
	int         out_ch;     /* box output (downstream) width             */
	uint8_t     config_block[32];  /* config-announce cdea 01 03 0010    */
	int         has_name;   /* 1 -> also emit the ASCII name frame       */
	uint8_t     name_block[32];    /* name frame cdea 04 01 001b (if any)*/
	/* The mixer identifies the MODEL from the cold-connect INVENTORY frames, not
	 * just the config-announce: the 0016/001a blocks differ per model, and some
	 * models emit an extra 0402000d frame. Byte-verified per model. */
	uint8_t     cc0014[32];        /* cold-connect cdea 04 03 0014       */
	uint8_t     cc0013[32];        /* cold-connect cdea 04 03 0013       */
	uint8_t     cc0016[32];        /* cold-connect cdea 04 03 0016       */
	uint8_t     cc001a[32];        /* cold-connect cdea 04 03 001a       */
	int         has_extra;  /* 1 -> also emit the cdea 04 02 000d frame  */
	uint8_t     extra_block[32];   /* cdea 04 02 000d (if any)           */
};
const struct reac_box_model *reac_box_model_by_token(const char *token);
const struct reac_box_model *reac_box_model_by_channels(int in_ch);
const struct reac_box_model *reac_box_model_table(size_t *count);

/* MASTER-side box RECOGNITION (the mirror of the slave emitter): given a raw
 * received frame, if it is a box config-announce (cdea 01 03 0010) whose
 * descriptor block matches a fixed-matrix row, return that model; else NULL.
 * "The matrix is law as a stagebox; as a mixer we read the frame and use the
 * matrix as the default" — a NULL means no known model, and the caller falls
 * back to the descriptor/width carried in the frame. PURE (no socket). */
const struct reac_box_model *reac_ctrl_identify_box(const uint8_t *frame, size_t len);

/* Config-announce (cdea 01 03 0010) — the SETUP DECLARATION the master enrolls
 * the box from. Byte-verified per model; the selector byte sets the displayed
 * model family. in_ch selects the fixed-matrix row (falls back to S-1608). */
size_t reac_ctrl_build_config_announce(uint8_t *out, const uint8_t master[6],
                                       const uint8_t src[6], uint16_t counter, int in_ch);
/* ASCII model-name frame (cdea 04 01 001b) — required for the 0x84 family so the
 * desk shows the exact model (e.g. "S-0808") instead of the generic family name.
 * Returns 0 (emits nothing) for models whose name comes from the selector alone
 * (the 0x82 / S-1608 family). */
size_t reac_ctrl_build_name_frame(uint8_t *out, const uint8_t master[6],
                                  const uint8_t src[6], uint16_t counter, int in_ch);
/* The extra cold-connect frame (cdea 04 02 000d) some models send (S-0808). The
 * mixer uses it, with the 0016/001a inventory, to determine the exact model.
 * Returns 0 (emits nothing) for models that don't send it (e.g. S-1608). */
size_t reac_ctrl_build_extra_frame(uint8_t *out, const uint8_t master[6],
                                   const uint8_t src[6], uint16_t counter, int in_ch);
/* The box cold-connect (cdea 04 03): the 32-byte control block over LIVE audio
 * [50:626] (the [38:66] region is per-frame audio, NOT device inventory). Audio is
 * planar float [ch][s], as build_upstream_filler; NULL planar -> silent. The master
 * echoes the control block verbatim as its grant. */
size_t reac_ctrl_build_coldconnect(uint8_t *out, const uint8_t master[6],
                                   const uint8_t src[6], uint16_t counter,
                                   int n_ch, float *const *planar, int ns);

/* The cdea 04 03 0013 cold-connect variant, interleaved with the 0014 by a real
 * box. Emitted raw (the 0013 block is not sum-to-0). */
size_t reac_ctrl_build_coldconnect_0013(uint8_t *out, const uint8_t master[6],
                                        const uint8_t src[6], uint16_t counter,
                                        int n_ch, float *const *planar, int ns);

/* The cdea 04 03 0016 and 001a cold-connect variants — the rest of the escalation
 * a real S-1608 sends (0014 -> 0013 -> 0016 -> 001a). They carry the fuller box
 * inventory the master needs to register the box in its REAC device list. Emitted
 * raw (byte-matched to a real S-1608, 2026-07-11). */
size_t reac_ctrl_build_coldconnect_0016(uint8_t *out, const uint8_t master[6],
                                        const uint8_t src[6], uint16_t counter,
                                        int n_ch, float *const *planar, int ns);
size_t reac_ctrl_build_coldconnect_001a(uint8_t *out, const uint8_t master[6],
                                        const uint8_t src[6], uint16_t counter,
                                        int n_ch, float *const *planar, int ns);

/* ---- Head-amp source control (op 04 03, record TAG 01 01) ----
 * Ground-truthed on a live M-200 driving an S-0808 + S-1608 (reac-captures/
 * m200-headamp-re/DECODE.md, 2026-07-17): op 04 03 is a RECORD CONTAINER, and
 * the record after the 12 12 marker is TAG(2) DATA(n) CKSUM(1). TAG 01 01 is
 * the console's preamp command, DATA = CH PARAM VALUE. CH is the WIRE channel:
 * model_base + (box_input - 1), model_base S-0808/S-4000S 0x00, S-1608 0x20.
 * Two nested checksums: the record TAG..CKSUM sums to 0x80 mod 256, and the
 * enclosing 32-byte block keeps the usual sum-to-0 at [49]. */
enum reac_headamp_param {
	REAC_HEADAMP_PHANTOM = 0x00,   /* +48V on/off (value 0|1) */
	REAC_HEADAMP_PAD     = 0x01,   /* -20 dB pad on/off (value 0|1) */
	REAC_HEADAMP_SENS    = 0x02,   /* sensitivity (value 0x00..0x37, 1 dB/step) */
};
#define REAC_HEADAMP_SENS_MAX 0x37

/* The head-amp WIRE-CHANNEL space: 0x00..0x2f, so 0x30 = 48 addressable channels.
 *
 * This is NOT libreac's REAC_MAX_CHANNELS (40). The two are DIFFERENT spaces and
 * conflating them is a bug (fixed 2026-07-17): REAC_MAX_CHANNELS is the count of
 * AUDIO slots carried in a downstream frame, whereas a head-amp record's CH is a
 * fabric wire channel = model_base + (box_input - 1), and the fabric runs to the
 * 0x2f ceiling (the same ceiling reac_master.c's chanmap ring already encodes as
 * REAC_M_FABRIC_RING = 48 channels + the 0xfe marker). A 16-input S-1608 based at
 * 0x20 occupies 0x20..0x2f = 32..47, so a table bounded by 40 silently REJECTED
 * that box's inputs 9..16 — its top half could never be given phantom/pad/sens. */
#define REAC_HEADAMP_MAX_CH 0x30

/* Build the head-amp command frame (master->box direction, downstream width:
 * a real console BROADCASTS these interleaved in its stream — pass the
 * broadcast MAC as the dst like every builder's first MAC arg). ch is the
 * already-resolved wire channel. Returns the frame length, or 0 on a bad
 * param/value combination. */
size_t reac_ctrl_build_headamp(uint8_t *out, const uint8_t master[6],
                               const uint8_t src[6], uint16_t counter,
                               uint8_t ch, uint8_t param, uint8_t value);

/* Stamp a head-amp record over the type [16:18] + control block [18:50] of an
 * ALREADY-BUILT downstream frame, preserving its audio [50:], counter and tail.
 * The MASTER emit path (reac_headamp_tx + the pacer) uses this to overlay a
 * head-amp command onto a FILLER slot without rebuilding the frame. Returns 0, or
 * -1 on a bad param/value (the frame is left untouched). */
int reac_ctrl_stamp_headamp(uint8_t *frame, uint8_t ch, uint8_t param, uint8_t value);

/* Human-readable head-amp parameter name ("phantom" / "pad" / "SENS", or "?"
 * for an unknown param) for logging a received or emitted record. */
const char *reac_headamp_param_name(uint8_t param);

/* Verify a HEADAMP record's INNER checksum. The record bytes TAG..CKSUM (frame
 * offsets [34..39], i.e. block-relative [16..21]) must sum to 0x80 mod 256. Call
 * BEFORE trusting a parsed head-amp CH/PARAM/VALUE so a corrupted knob record is
 * never surfaced or acted on. The frame must already have parsed as
 * REAC_CTRL_HEADAMP (it reads the fixed record offsets). Returns 0 when valid,
 * -1 when the record checksum is wrong. */
int reac_ctrl_headamp_record_verify(const uint8_t *frame);

/* SENS VALUE <-> dB (pad-relative, 1 dB/step): dB = -10 - value + (pad ? 20 : 0).
 * pad off: 0x00 = -10 dBu .. 0x37 = -65 dBu; pad on: 0x00 = +10 .. 0x37 = -45.
 * sens_value clamps into 0x00..0x37. */
int     reac_headamp_sens_db(uint8_t value, int pad_on);
uint8_t reac_headamp_sens_value(int db, int pad_on);

#endif /* REAC_CTRL_H */
