// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The head-amp door's READ side — docs/design/specs/2026-09-14-headamp-as-node-
 * params.md §3a, RULED by the operator 2026-09-14.
 *
 * Two halves, both offline (no socket, no graph, no RT privilege).
 *
 * 1. THE WIRE. A `Props` pod carrying `reac.headamp.32.phantom = 1` driven through
 *    reac_headamp_prop_parse -> reac_pacer_headamp_set -> reac_pacer_headamp_drain
 *    -> reac_headamp_tx_next -> reac_ctrl_stamp_headamp — the exact chain
 *    reac_pacer.c runs on an ESTABLISHED FILLER slot — and the emitted frame's
 *    control block byte-compared to LIBREAC'S CAPTURED GOLDEN: FX_L4_HEADAMP, a
 *    TAG 0x0101 record taken off an M-200 commanding an S-1608
 *    (m200i-none-48k-clean__m200-s1608-realbox-establish-2026-07-11.pcap), whose
 *    record bytes are CH 0x20, PARAM 0x00, VALUE 0x01. The fixture is an S-1608 at
 *    base 32, so the same comparison pins the base law: box input 1 IS wire
 *    channel 32 and a key that says 32 must put 0x20 on the wire.
 *
 *    The golden bytes are the ORACLE, and that is the whole point. Asserting that
 *    the parse returned what the builder built would be a self-consistent loop
 *    that stays green over a wire format nothing on the far end accepts. The
 *    bytes come from libreac's own fixture file rather than being retyped here as
 *    a second copy — libreac and the .ksy own the protocol.
 *
 * 2. THE ANSWER. Every refusal code produced and READ BACK as the string a client
 *    reads off the node, plus the capability decision that composes it and the
 *    asserted cell list a second client renders a switch from. Counting a write
 *    that was refused as a success is the exact defect this half exists against.
 *
 *    A HONEST NOTE ON WHAT EACH CODE PROVES. `no-box`, `box-master` and `no-base`
 *    are STANDING facts of a segment: they are true with or without a write, so
 *    reading one back after a write does not by itself prove the write was seen.
 *    `bad-key` and `out-of-range` are the opposite — nothing but an arriving cell
 *    can produce them — so those two are what pin the write path, and they are
 *    asserted here against a capability that is PRESENT. The capability codes are
 *    proved the other way: by changing the segment's facts and requiring the code
 *    to follow.
 *
 * What is NOT here, and belongs to the rig: a gain change MEASURED on a box's
 * audio. Bytes on a frame are not decibels at an XLR. */
#include <reac/transport/reac_pacer.h>
#include "reac_headamp_prop.h"
#include "reac_headamp_state.h"

#include <reac/reac.h>
#include <reac/reac_ctrl.h>
#include <reac/reac_encode.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* ---- libreac's captured golden, copied verbatim from its tests/ctrl_fixtures.inc
 * ("link 4 SINGLE, TAG 0x0101 — a head-amp record, CH PARAM VALUE"). It is a
 * frame's [16:50] slice: the 2-byte type + the 32-byte control block. */
static const uint8_t FX_L4_HEADAMP[34] = {
	0xcd, 0xea, 0x04, 0x03, 0x00, 0x13, 0x00, 0x02, 0x00, 0xfe, 0x0e, 0xf0,
	0x41, 0x0a, 0x00, 0x00, 0x12, 0x12, 0x01, 0x01, 0x20, 0x00, 0x01, 0x5d,
	0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
};
#define CTRL_OFF REAC_TYPED_BLOCK_OFF          /* where that slice starts in a downstream frame */

static const uint8_t SRC[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };

/* ---- Props pod helpers, the same shape test_reac_headamp_prop.c uses ------ */

static void begin_props(struct spa_pod_builder *b, struct spa_pod_frame *obj,
                        struct spa_pod_frame *st)
{
	spa_pod_builder_push_object(b, obj, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(b, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(b, st);
}

static const struct spa_pod *end_props(struct spa_pod_builder *b,
                                       struct spa_pod_frame *obj,
                                       struct spa_pod_frame *st)
{
	spa_pod_builder_pop(b, st);
	return (const struct spa_pod *)spa_pod_builder_pop(b, obj);
}

static void kv_int(struct spa_pod_builder *b, const char *k, int v)
{
	spa_pod_builder_string(b, k);
	spa_pod_builder_int(b, v);
}

/* ---- part 1: the wire ---------------------------------------------------- */

/* A `struct reac_pacer` with the head-amp path initialised and NO socket: the
 * subset of reac_pacer_open's work that has no I/O in it (test_reac_rate_cfg.c's
 * bare_pacer_init, narrowed to what the command ring and the send table touch). */
static void bare_pacer_headamp_init(struct reac_pacer *p)
{
	memset(p, 0, sizeof *p);
	p->handle = NULL;
	memcpy(p->src, SRC, 6);
	reac_headamp_tx_init(&p->headamp);
	atomic_init(&p->ha_cmd_head, 0);
	atomic_init(&p->ha_cmd_tail, 0);
}

/* Emit one downstream FILLER exactly as the pacer's ESTABLISHED slot does: build
 * the audio frame, then overlay whatever the head-amp scheduler says this slot
 * carries. Returns 1 when a record was stamped. */
static int emit_filler(struct reac_pacer *p, uint8_t *frame, uint16_t counter)
{
	CHK(reac_downstream_build(frame, NULL, 0, REAC_SAMPLES_PER_PKT, counter, p->src)
	    == REAC_FRAME_BYTES);
	if (!p->headamp.active && !p->headamp.replay_width)
		return 0;
	uint8_t ch, param, value;
	if (!reac_headamp_tx_next(&p->headamp, &ch, &param, &value))
		return 0;
	return reac_ctrl_stamp_headamp(frame, ch, param, value) == 0;
}

/* THE ACCEPTANCE LINE: a Props write becomes the golden's bytes on the wire. */
static int test_props_write_emits_the_golden_record(void)
{
	uint8_t buf[512];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
	struct spa_pod_frame obj, st;
	begin_props(&b, &obj, &st);
	/* Wire channel 32 = 0x20 = an S-1608 at base 32, box input 1. */
	kv_int(&b, "reac.headamp.32.phantom", 1);
	const struct spa_pod *pod = end_props(&b, &obj, &st);

	struct reac_headamp_setting ha[REAC_HEADAMP_MAX_CH * REAC_HEADAMP_NPARAMS];
	struct reac_headamp_prop_result res;
	int n = reac_headamp_prop_parse_result(pod, ha, (int)(sizeof ha / sizeof ha[0]),
	                                       &res);
	CHK(n == 1 && res.keys == 1 && res.refusal == REAC_HEADAMP_REFUSE_NONE);

	struct reac_pacer p;
	bare_pacer_headamp_init(&p);
	for (int i = 0; i < n; i++)
		CHK(reac_pacer_headamp_set(&p, ha[i].ch, ha[i].param, ha[i].value) == 1);
	CHK(reac_pacer_headamp_drain(&p) == 1);   /* the RT side applies the cell */

	uint8_t frame[REAC_FRAME_BYTES];
	CHK(emit_filler(&p, frame, 0x0e11) == 1);

	/* THE ORACLE. Byte-for-byte against the capture, inner checksum (0x5d)
	 * included — a value or a channel off by one moves that byte too. */
	CHK(memcmp(frame + CTRL_OFF, FX_L4_HEADAMP, sizeof FX_L4_HEADAMP) == 0);
	CHK(reac_ctrl_headamp_record_verify(frame) == 0);
	CHK(reac_ctrl_checksum_verify(frame) == 0);
	CHK(frame[REAC_FRAME_BYTES - 2] == REAC_END_MARKER_0 && frame[REAC_FRAME_BYTES - 1] == REAC_END_MARKER_1);

	/* The edge is consumed: a master that has said it once goes quiet, as a real
	 * M-200 does (no re-assert cadence is set here). */
	uint8_t next[REAC_FRAME_BYTES];
	CHK(emit_filler(&p, next, 0x0e12) == 0);

	/* THE NEGATIVE CONTROL. The same comparison must be ABLE to fail: a write of
	 * the neighbouring channel produces a control block the golden does not
	 * match. Without this the memcmp above could be passing on a frame the stamp
	 * never touched. */
	CHK(reac_pacer_headamp_set(&p, 33, REAC_HEADAMP_PHANTOM, 1) == 1);
	CHK(reac_pacer_headamp_drain(&p) == 1);
	uint8_t other[REAC_FRAME_BYTES];
	CHK(emit_filler(&p, other, 0x0e13) == 1);
	CHK(memcmp(other + CTRL_OFF, FX_L4_HEADAMP, sizeof FX_L4_HEADAMP) != 0);
	CHK(reac_ctrl_headamp_record_verify(other) == 0);   /* still a valid record */
	return 0;
}

/* ---- part 2: the answer -------------------------------------------------- */

static int test_capability_decision(void)
{
	/* An enrolled S-1608: 16 preamps, strap 32. The only accepting case. */
	CHK(reac_headamp_cfg_decide(0, REAC_BOX_S1608_IN, 32) == REAC_HEADAMP_REFUSE_NONE);
	CHK(strcmp(reac_headamp_cfg_state(0, 16, 32), REAC_HEADAMP_STATE_APPLIED) == 0);

	/* No model recognised yet. */
	CHK(reac_headamp_cfg_decide(0, 0, -1) == REAC_HEADAMP_REFUSE_NO_BOX);
	CHK(strcmp(reac_headamp_cfg_state(0, 0, -1), REAC_HEADAMP_STATE_UNAVAILABLE) == 0);

	/* A box on M. It ALSO has channels == 0, and the more specific fact has to
	 * win — `no-box` cannot be rendered as "the box is master, its preamps are
	 * preconfigured", and that sentence is the reason this code exists. */
	CHK(reac_headamp_cfg_decide(1, 0, -1) == REAC_HEADAMP_REFUSE_BOX_MASTER);
	CHK(reac_headamp_cfg_decide(1, REAC_BOX_S1608_IN, 32) == REAC_HEADAMP_REFUSE_BOX_MASTER);
	CHK(strcmp(reac_headamp_cfg_state(1, 0, -1), REAC_HEADAMP_STATE_UNAVAILABLE) == 0);

	/* A recognised box with no announced strap: there is no wire address, and 0
	 * is not a safe guess — it would address an S-1608's preamps 32 slots low. */
	CHK(reac_headamp_cfg_decide(0, REAC_BOX_S1608_IN, -1) == REAC_HEADAMP_REFUSE_NO_BASE);

	/* The codes as a client reads them. */
	CHK(strcmp(reac_headamp_refuse_code(REAC_HEADAMP_REFUSE_NONE), "none") == 0);
	CHK(strcmp(reac_headamp_refuse_code(REAC_HEADAMP_REFUSE_NO_BOX), "no-box") == 0);
	CHK(strcmp(reac_headamp_refuse_code(REAC_HEADAMP_REFUSE_BOX_MASTER), "box-master") == 0);
	CHK(strcmp(reac_headamp_refuse_code(REAC_HEADAMP_REFUSE_NO_BASE), "no-base") == 0);
	CHK(strcmp(reac_headamp_refuse_code(REAC_HEADAMP_REFUSE_BAD_KEY), "bad-key") == 0);
	CHK(strcmp(reac_headamp_refuse_code(REAC_HEADAMP_REFUSE_OUT_OF_RANGE),
	           "out-of-range") == 0);
	return 0;
}

/* The door's composed answer, assembled the way sink_publish_headamp_props does:
 * the capability if there is one to report, else the last write's own outcome.
 * Kept in one place here so every case below reads the same string a client would
 * read off `reac.headamp.refused`. */
static const char *door_refused(int box_master, int channels, int base,
                                enum reac_headamp_refuse write_refusal)
{
	enum reac_headamp_refuse cap = reac_headamp_cfg_decide(box_master, channels, base);
	return reac_headamp_refuse_code(cap != REAC_HEADAMP_REFUSE_NONE ? cap : write_refusal);
}

/* Parse one Props object holding a single (key, int value) pair. */
static int parse_one(uint8_t *buf, size_t bufsz, const char *key, int value,
                     struct reac_headamp_setting *out, int max,
                     struct reac_headamp_prop_result *res)
{
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, bufsz);
	struct spa_pod_frame obj, st;
	begin_props(&b, &obj, &st);
	kv_int(&b, key, value);
	const struct spa_pod *pod = end_props(&b, &obj, &st);
	return reac_headamp_prop_parse_result(pod, out, max, res);
}

/* EVERY REFUSAL CODE, PRODUCED AND READ BACK. */
static int test_every_refusal_is_visible(void)
{
	uint8_t buf[512];
	struct reac_headamp_setting out[REAC_HEADAMP_MAX_CH * REAC_HEADAMP_NPARAMS];
	struct reac_headamp_prop_result res;
	const int MAX = (int)(sizeof out / sizeof out[0]);

	/* -- the two codes only an arriving write can produce, against a segment
	 * whose capability is PRESENT (an enrolled S-1608 at base 32). These are what
	 * prove the write was actually seen. */

	/* bad-key: the key is not a cell address. */
	CHK(parse_one(buf, sizeof buf, "reac.headamp.x.phantom", 1, out, MAX, &res) == 0);
	CHK(res.keys == 1 && res.refusal == REAC_HEADAMP_REFUSE_BAD_KEY);
	CHK(strcmp(door_refused(0, 16, 32, res.refusal), "bad-key") == 0);

	CHK(parse_one(buf, sizeof buf, "reac.headamp.32.gain", 1, out, MAX, &res) == 0);
	CHK(res.keys == 1 && res.refusal == REAC_HEADAMP_REFUSE_BAD_KEY);
	CHK(strcmp(door_refused(0, 16, 32, res.refusal), "bad-key") == 0);

	/* out-of-range: it addresses a cell, but not one the wire admits. The bound
	 * is the head-amp WIRE-channel space (0x00..0x2f), so 48 is the first
	 * invalid channel — 40 is a real one (an S-1608 based at 0x20 owns
	 * 0x20..0x2f). */
	CHK(parse_one(buf, sizeof buf, "reac.headamp.48.phantom", 1, out, MAX, &res) == 0);
	CHK(res.keys == 1 && res.refusal == REAC_HEADAMP_REFUSE_OUT_OF_RANGE);
	CHK(strcmp(door_refused(0, 16, 32, res.refusal), "out-of-range") == 0);

	/* sens above REAC_HEADAMP_SENS_MAX — the travel `reac.headamp.sens.max`
	 * publishes, so a client that renders the received range never sends this. */
	CHK(parse_one(buf, sizeof buf, "reac.headamp.32.sens",
	              REAC_HEADAMP_SENS_MAX + 1, out, MAX, &res) == 0);
	CHK(res.keys == 1 && res.refusal == REAC_HEADAMP_REFUSE_OUT_OF_RANGE);
	CHK(strcmp(door_refused(0, 16, 32, res.refusal), "out-of-range") == 0);

	/* phantom is a toggle: 2 is not a third state. */
	CHK(parse_one(buf, sizeof buf, "reac.headamp.32.phantom", 2, out, MAX, &res) == 0);
	CHK(res.refusal == REAC_HEADAMP_REFUSE_OUT_OF_RANGE);

	/* -- the three standing codes, each proved by CHANGING the segment's facts
	 * and requiring the answer to follow. A well-formed write is used every time,
	 * so what is being read back is the capability and nothing else. */
	CHK(parse_one(buf, sizeof buf, "reac.headamp.32.phantom", 1, out, MAX, &res) == 1);
	CHK(res.keys == 1 && res.refusal == REAC_HEADAMP_REFUSE_NONE);

	CHK(strcmp(door_refused(0, 0, -1, res.refusal), "no-box") == 0);
	CHK(strcmp(door_refused(1, 0, -1, res.refusal), "box-master") == 0);
	CHK(strcmp(door_refused(0, 16, -1, res.refusal), "no-base") == 0);
	CHK(strcmp(door_refused(0, 16, 32, res.refusal), "none") == 0);

	/* -- and the absence that is not a refusal: a Props object that carries no
	 * head-amp key at all (a volume write) leaves the answer alone. `keys == 0`
	 * is what tells those two apart, and without it a refusal would be reported
	 * for every volume change on the desk. */
	CHK(parse_one(buf, sizeof buf, "some.other.control", 99, out, MAX, &res) == 0);
	CHK(res.keys == 0 && res.refusal == REAC_HEADAMP_REFUSE_NONE);

	/* -- a mixed write: the good cell lands AND the bad one is still reported.
	 * A parse that let the later success erase the earlier refusal would be the
	 * blind pass this whole property exists against. */
	{
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
		struct spa_pod_frame obj, st;
		begin_props(&b, &obj, &st);
		kv_int(&b, "reac.headamp.32.sens", 0x38);      /* refused: out of range */
		kv_int(&b, "reac.headamp.33.sens", 0x20);      /* accepted              */
		const struct spa_pod *pod = end_props(&b, &obj, &st);
		CHK(reac_headamp_prop_parse_result(pod, out, MAX, &res) == 1);
		CHK(res.keys == 2 && res.n == 1);
		CHK(out[0].ch == 33 && out[0].value == 0x20);
		CHK(res.refusal == REAC_HEADAMP_REFUSE_OUT_OF_RANGE);
	}
	return 0;
}

/* THE READBACK: what a second client renders a switch from, without having
 * written anything itself. */
static int test_asserted_readback(void)
{
	struct reac_headamp_asserted t;
	char buf[REAC_HEADAMP_ASSERTED_MAX];

	/* Nothing set: the empty string, never a stray separator. */
	reac_headamp_asserted_init(&t);
	CHK(reac_headamp_asserted_render(&t, buf, sizeof buf) == 0);
	CHK(strcmp(buf, "") == 0);

	/* The spec's own example, cells deliberately written out of order to pin
	 * that the render is sorted by channel then param rather than by arrival. */
	CHK(reac_headamp_asserted_set(&t, 35, REAC_HEADAMP_PHANTOM, 0) == 0);
	CHK(reac_headamp_asserted_set(&t, 34, REAC_HEADAMP_SENS, 52) == 0);
	CHK(reac_headamp_asserted_set(&t, 34, REAC_HEADAMP_PHANTOM, 1) == 0);
	CHK(reac_headamp_asserted_render(&t, buf, sizeof buf) == strlen(buf));
	CHK(strcmp(buf, "34:0=1,34:2=52,35:0=0") == 0);

	/* A DELIBERATE 0 IS A SETTING, not an absence: 35:0=0 above is phantom
	 * turned OFF, and a reader must be able to see that it was turned off rather
	 * than never touched. */
	CHK(t.set[35][REAC_HEADAMP_PHANTOM] == 1 && t.value[35][REAC_HEADAMP_PHANTOM] == 0);

	/* A later write replaces the cell; the list does not grow a duplicate. */
	CHK(reac_headamp_asserted_set(&t, 34, REAC_HEADAMP_SENS, 20) == 0);
	reac_headamp_asserted_render(&t, buf, sizeof buf);
	CHK(strcmp(buf, "34:0=1,34:2=20,35:0=0") == 0);

	/* Out-of-range cells never enter the table, so the readback can never claim
	 * a value the send table would have refused. */
	CHK(reac_headamp_asserted_set(&t, REAC_HEADAMP_MAX_CH, REAC_HEADAMP_PHANTOM, 1) == -1);
	CHK(reac_headamp_asserted_set(&t, 34, REAC_HEADAMP_SENS,
	                              REAC_HEADAMP_SENS_MAX + 1) == -1);
	CHK(reac_headamp_asserted_set(&t, 34, REAC_HEADAMP_PAD, 2) == -1);
	reac_headamp_asserted_render(&t, buf, sizeof buf);
	CHK(strcmp(buf, "34:0=1,34:2=20,35:0=0") == 0);

	/* A full table fits the buffer the node publishes from — the cap is derived,
	 * not guessed, so a wider box can never truncate the readback silently. */
	reac_headamp_asserted_init(&t);
	for (int ch = 0; ch < REAC_HEADAMP_MAX_CH; ch++) {
		CHK(reac_headamp_asserted_set(&t, (uint8_t)ch, REAC_HEADAMP_PHANTOM, 1) == 0);
		CHK(reac_headamp_asserted_set(&t, (uint8_t)ch, REAC_HEADAMP_PAD, 1) == 0);
		CHK(reac_headamp_asserted_set(&t, (uint8_t)ch, REAC_HEADAMP_SENS,
		                              REAC_HEADAMP_SENS_MAX) == 0);
	}
	size_t want = reac_headamp_asserted_render(&t, buf, sizeof buf);
	CHK(want == strlen(buf));               /* nothing was truncated */
	CHK(want < sizeof buf);
	return 0;
}

/* ONE CLIENT WRITES, ANOTHER READS. The door's own sequence: a Props write
 * arrives, the accepted cells go to the pacer AND into the asserted mirror in the
 * same statement, and a client that never wrote anything renders the switch from
 * the string. The two copies are compared against each other at the end, because
 * a readback that drifts from what is on the wire is worse than none. */
static int test_one_client_writes_another_reads(void)
{
	struct reac_pacer p;
	struct reac_headamp_asserted mirror;
	struct reac_headamp_setting ha[REAC_HEADAMP_MAX_CH * REAC_HEADAMP_NPARAMS];
	struct reac_headamp_prop_result res;
	uint8_t buf[512];
	const int MAX = (int)(sizeof ha / sizeof ha[0]);

	bare_pacer_headamp_init(&p);
	reac_headamp_asserted_init(&mirror);

	/* Nothing written yet: the reader sees an empty list — not a guess, not a
	 * default, and not a switch it has no business drawing as `on`. */
	char rendered[REAC_HEADAMP_ASSERTED_MAX];
	reac_headamp_asserted_render(&mirror, rendered, sizeof rendered);
	CHK(strcmp(rendered, "") == 0);

	/* CLIENT A writes two cells on an enrolled S-1608 (base 32). */
	{
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
		struct spa_pod_frame obj, st;
		begin_props(&b, &obj, &st);
		kv_int(&b, "reac.headamp.32.phantom", 1);
		kv_int(&b, "reac.headamp.32.sens", 52);
		const struct spa_pod *pod = end_props(&b, &obj, &st);
		int n = reac_headamp_prop_parse_result(pod, ha, MAX, &res);
		CHK(n == 2 && res.keys == 2);
		CHK(reac_headamp_cfg_decide(0, REAC_BOX_S1608_IN, 32) == REAC_HEADAMP_REFUSE_NONE);
		for (int i = 0; i < n; i++) {
			CHK(reac_pacer_headamp_set(&p, ha[i].ch, ha[i].param, ha[i].value) == 1);
			CHK(reac_headamp_asserted_set(&mirror, ha[i].ch, ha[i].param,
			                              ha[i].value) == 0);
		}
	}

	/* CLIENT B, which wrote nothing, reads the cells off the property. */
	reac_headamp_asserted_render(&mirror, rendered, sizeof rendered);
	CHK(strcmp(rendered, "32:0=1,32:2=52") == 0);

	/* AND THE READBACK AGREES WITH THE WIRE. Drain the ring into the real send
	 * table and require every asserted cell to be the value that table would put
	 * on the wire — the check that makes the mirror a readback rather than a
	 * second, independently drifting ledger. */
	CHK(reac_pacer_headamp_drain(&p) == 2);
	for (int ch = 0; ch < REAC_HEADAMP_MAX_CH; ch++)
		for (int prm = 0; prm < REAC_HEADAMP_NPARAMS; prm++)
			if (mirror.set[ch][prm])
				CHK(reac_headamp_tx_effective(&p.headamp, (uint8_t)ch,
				                              (uint8_t)prm) ==
				    mirror.value[ch][prm]);

	/* AN ESTABLISHMENT RE-PUSH CHANGES NOTHING ABOUT THE READBACK. Arming the
	 * complete scene is what restores a power-cycled box's pins; the cells the
	 * operator set are still the cells the client reads afterwards. */
	reac_headamp_tx_arm_scene(&p.headamp, 32, REAC_BOX_S1608_IN);
	char after[REAC_HEADAMP_ASSERTED_MAX];
	reac_headamp_asserted_render(&mirror, after, sizeof after);
	CHK(strcmp(after, rendered) == 0);
	return 0;
}

int main(void)
{
	if (test_props_write_emits_the_golden_record())
		return 1;
	if (test_capability_decision())
		return 1;
	if (test_every_refusal_is_visible())
		return 1;
	if (test_asserted_readback())
		return 1;
	if (test_one_client_writes_another_reads())
		return 1;
	printf("reac_headamp_state: OK\n");
	return 0;
}
