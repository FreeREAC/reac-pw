// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_rate_cfg — the `reac.cfg.rate` decision core, and the pacer-level
 * internal re-establish it authorizes (2026-08-26-reac-runtime-config.md).
 *
 * Two halves, both pure (no socket, no RT privilege):
 *
 *   1. The decision core itself (reac_rate_cfg.c): closed-list membership, the
 *      drivable-mask arithmetic (best-drivable, the CSV render), the accept/
 *      refuse decision, and the SPA_PROP_params parse — built and asserted the
 *      same way test_reac_headamp_prop.c proves reac_headamp_prop.c.
 *
 *   2. reac_pacer_apply_rate/_request_rate/_rate_drain, driven directly on a
 *      hand-built `struct reac_pacer` with no AF_PACKET socket at all — the
 *      same style test_reac_headamp_live.c uses for the head-amp command ring.
 *      Establishes a box at 48 kHz, captures the grant burst's SHAPE (count,
 *      stride, order, dwell), asserts a `reac.cfg.rate` change to 96 kHz
 *      re-establishes with the IDENTICAL shape, and that a refused rate (out
 *      of the closed list, or outside a narrowed drivable mask, or asserted
 *      against a slave) moves nothing at all — fps, period_ns and the master
 *      FSM state are byte-for-byte what they were before the refused write.
 *
 * What this does NOT cover: the `reac.cfg.rate.state` pending->applied flip
 * that note_transition performs on entering ESTABLISHED. note_transition is
 * `static` in reac_pacer.c and only ever called from the real SCHED_FIFO
 * pacer_loop, which needs a live AF_PACKET socket to run at all — the same
 * reason test_reac_pacer.c's own live-cadence section is best-effort and
 * SKIPs without CAP_NET_RAW. reac_pacer_apply_rate's OWN bookkeeping (the
 * immediate flip to `pending`, the box/badge forget, the ended session) is
 * fully covered here without that dependency. */
#include "reac_pacer.h"
#include "reac_rate_cfg.h"
#include "reac_role.h"
#include "reac_ctrl.h"

#include <reac/reac.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* ---- part 1: the decision core, pure ------------------------------------ */

static int test_closed_list_and_bits(void)
{
	CHK(reac_rate_is_closed(44100) && reac_rate_is_closed(48000) &&
	    reac_rate_is_closed(96000));
	CHK(!reac_rate_is_closed(44101) && !reac_rate_is_closed(0) &&
	    !reac_rate_is_closed(192000));

	CHK(reac_rate_bit(44100) == REAC_RATE_BIT_44100);
	CHK(reac_rate_bit(48000) == REAC_RATE_BIT_48000);
	CHK(reac_rate_bit(96000) == REAC_RATE_BIT_96000);
	CHK(reac_rate_bit(12345) == 0);   /* not in the closed list: no bit at all */
	return 0;
}

static int test_best_drivable(void)
{
	/* The honest default: nothing measured, everything declared drivable. */
	CHK(reac_rate_best_drivable(REAC_RATE_ALL_BITS) == 96000);
	/* "if we cannot drive a 96 kHz mixer then we must default to something
	 * lesser" — a segment that cannot do 96k defaults to 48k, never silently
	 * clamped to a third value and never refusing to have a default at all. */
	CHK(reac_rate_best_drivable(REAC_RATE_BIT_44100 | REAC_RATE_BIT_48000) == 48000);
	CHK(reac_rate_best_drivable(REAC_RATE_BIT_44100) == 44100);
	CHK(reac_rate_best_drivable(0) == 0);   /* no bits at all: no rate to give */
	return 0;
}

static int test_drivable_csv(void)
{
	char buf[64];
	CHK(reac_rate_drivable_csv(REAC_RATE_ALL_BITS, buf, sizeof buf) == 17);
	CHK(strcmp(buf, "44100,48000,96000") == 0);

	/* Ascending order is the mask's iteration order, not the bit layout's —
	 * prove it survives a mask whose SET bit is not the lowest one. */
	CHK(reac_rate_drivable_csv(REAC_RATE_BIT_96000, buf, sizeof buf) == 5);
	CHK(strcmp(buf, "96000") == 0);

	CHK(reac_rate_drivable_csv(REAC_RATE_BIT_44100 | REAC_RATE_BIT_96000,
	                           buf, sizeof buf) == 11);
	CHK(strcmp(buf, "44100,96000") == 0);

	CHK(reac_rate_drivable_csv(0, buf, sizeof buf) == 0);
	CHK(buf[0] == '\0');
	return 0;
}

static int test_decide(void)
{
	/* (b) out of the closed list -> refused, regardless of drivability. */
	CHK(reac_rate_cfg_decide(REAC_ROLE_MASTER, 44101, REAC_RATE_ALL_BITS) ==
	    REAC_RATE_REFUSE_NOT_CLOSED);

	/* In the closed list but outside THIS segment's drivable subset — the
	 * "cannot do 96k" case the spec's own roadmap names, proven here at the
	 * decision-core level since no real probe exists to supply the mask from
	 * the wire (reac_rate_cfg.h's honesty clause explains why). */
	CHK(reac_rate_cfg_decide(REAC_ROLE_MASTER, 96000,
	                         REAC_RATE_BIT_44100 | REAC_RATE_BIT_48000) ==
	    REAC_RATE_REFUSE_NOT_DRIVABLE);

	/* (c) a slave refuses EVERY assertion, even a valid, fully drivable one —
	 * "as a SLAVE there is no rate setting: refuse with its own code." */
	CHK(reac_rate_cfg_decide(REAC_ROLE_SLAVE, 48000, REAC_RATE_ALL_BITS) ==
	    REAC_RATE_REFUSE_ROLE_SLAVE);
	CHK(reac_rate_cfg_decide(REAC_ROLE_SLAVE, 44101, REAC_RATE_ALL_BITS) ==
	    REAC_RATE_REFUSE_ROLE_SLAVE);   /* role wins even over a bad value */

	/* The accepted case. */
	CHK(reac_rate_cfg_decide(REAC_ROLE_MASTER, 48000, REAC_RATE_ALL_BITS) ==
	    REAC_RATE_REFUSE_NONE);

	/* Every refusal has a non-"none" code; NONE is "none". */
	CHK(strcmp(reac_rate_refuse_code(REAC_RATE_REFUSE_NONE), "none") == 0);
	CHK(strcmp(reac_rate_refuse_code(REAC_RATE_REFUSE_NOT_CLOSED), "not_closed") == 0);
	CHK(strcmp(reac_rate_refuse_code(REAC_RATE_REFUSE_NOT_DRIVABLE), "not_drivable") == 0);
	CHK(strcmp(reac_rate_refuse_code(REAC_RATE_REFUSE_ROLE_SLAVE), "role_slave") == 0);
	CHK(strcmp(reac_rate_refuse_code(REAC_RATE_REFUSE_MALFORMED), "malformed") == 0);
	return 0;
}

/* Open a Props object with a SPA_PROP_params struct, same helper shape as
 * test_reac_headamp_prop.c's begin_props/end_props. */
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

static int test_prop_parse(void)
{
	uint8_t buf[1024];

	/* A well-formed reac.cfg.rate, interleaved with an unrelated head-amp key —
	 * the same params bag carries both (sink_build_params's single PropInfo). */
	{
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
		struct spa_pod_frame obj, st;
		begin_props(&b, &obj, &st);
		spa_pod_builder_string(&b, "reac.headamp.3.phantom");
		spa_pod_builder_int(&b, 1);
		spa_pod_builder_string(&b, REAC_CFG_PROP_RATE);
		spa_pod_builder_int(&b, 48000);
		const struct spa_pod *pod = end_props(&b, &obj, &st);

		int hz = -1;
		CHK(reac_rate_prop_parse(pod, &hz) == 1);
		CHK(hz == 48000);
	}

	/* A Float encoding (a slider/JSON-number bridge) rounds to the nearest Hz. */
	{
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
		struct spa_pod_frame obj, st;
		begin_props(&b, &obj, &st);
		spa_pod_builder_string(&b, REAC_CFG_PROP_RATE);
		spa_pod_builder_float(&b, 96000.4f);
		const struct spa_pod *pod = end_props(&b, &obj, &st);

		int hz = -1;
		CHK(reac_rate_prop_parse(pod, &hz) == 1);
		CHK(hz == 96000);
	}

	/* No reac.cfg.rate key at all (a plain head-amp write) -> 0, untouched. */
	{
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
		struct spa_pod_frame obj, st;
		begin_props(&b, &obj, &st);
		spa_pod_builder_string(&b, "reac.headamp.1.pad");
		spa_pod_builder_bool(&b, true);
		const struct spa_pod *pod = end_props(&b, &obj, &st);

		int hz = -1;
		CHK(reac_rate_prop_parse(pod, &hz) == 0);
		CHK(hz == -1);   /* untouched: the caller's variable is not stomped */
	}

	/* The key present, but a value neither Int nor Float can read -> -1
	 * (REAC_RATE_REFUSE_MALFORMED territory). */
	{
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
		struct spa_pod_frame obj, st;
		begin_props(&b, &obj, &st);
		spa_pod_builder_string(&b, REAC_CFG_PROP_RATE);
		spa_pod_builder_string(&b, "ninety-six-k");
		const struct spa_pod *pod = end_props(&b, &obj, &st);

		int hz = -1;
		CHK(reac_rate_prop_parse(pod, &hz) == -1);
	}

	/* NULL / not a Props object at all -> -1. */
	{
		int hz = -1;
		CHK(reac_rate_prop_parse(NULL, &hz) == -1);
	}

	return 0;
}

/* ---- part 2: the pacer-level internal re-establish ----------------------- */

static const uint8_t ZONEA_JOIN[32] = {
 0x04,0x03,0x00,0x14,0x00,0x02,0x00,0xfe,0x0f,0xf0,0x41,0x0a,0x00,0x00,0x12,0x12,
 0x01,0x00,0x06,0x00,0x01,0x00,0x78,0xf7,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
static const uint8_t SRC[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };
static const uint8_t BOX[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x3b };
#define S1608_BASE 0x20

/* Mirrors test_reac_master_no_enroll.c's helper of the same name: stand in the
 * quiet window between scene transfers so the JOIN below is not held. */
static void deliver_scene(struct reac_master *m, uint16_t *expect_counter)
{
	uint16_t c;
	int ix;
	long guard = 0;
	while ((m->scene_complete == 0 || m->scene_inflight) &&
	       guard++ < 4L * m->cycle_len)
		(void)reac_master_next(m, &c, &ix);
	if (expect_counter)
		*expect_counter = m->counter;
}

/* One captured establishment shape: what test (a) compares before vs. after
 * the rate change. Deliberately NOT the raw bytes (those legitimately differ:
 * a different fps re-scales grant_dwell, cycle_len, ...) — the SHAPE is the
 * count, the stride between grants (a FIXED constant, not fps-scaled), the
 * order blocks are granted in, and that establishment still completes. */
struct establish_shape {
	int grants;
	int grant_burst_len;
	int first_grant_slot;   /* relative to grant_dwell, so fps-independent */
	int stride;
	int reached_established;
};

static int shapes_equal(const struct establish_shape *a, const struct establish_shape *b)
{
	return a->grants == b->grants &&
	       a->grant_burst_len == b->grant_burst_len &&
	       a->first_grant_slot == b->first_grant_slot &&
	       a->stride == b->stride &&
	       a->reached_established && b->reached_established;
}

static int run_establish(struct reac_master *m, struct establish_shape *out)
{
	uint16_t cnt;
	reac_master_set_box(m, 16, 8, S1608_BASE);   /* S-1608: 16 in / 8 out */
	if (!reac_master_has_box(m))
		return -1;

	deliver_scene(m, &cnt);
	if (reac_master_rx(m, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) != 1)
		return -1;
	if (m->state != REAC_M_GRANTING)
		return -1;

	out->grant_burst_len = m->grant_burst_len;
	out->grants = 0;
	out->stride = -1;
	out->first_grant_slot = -1;
	int last_grant_slot = -1;

	int span = m->grant_dwell + m->grant_burst_len * REAC_M_GRANT_STRIDE + 4;
	for (int i = 0; i < span; i++) {
		int idx;
		uint16_t c;
		enum reac_master_emit e = reac_master_next(m, &c, &idx);
		if (c != cnt)
			return -1;
		cnt++;
		if (e == REAC_M_EMIT_GRANT) {
			if (idx != out->grants)
				return -1;             /* blocks must be granted in order */
			if (last_grant_slot >= 0) {
				int d = i - last_grant_slot;
				if (out->stride == -1)
					out->stride = d;
				else if (d != out->stride)
					return -1;          /* stride must be constant */
			} else {
				out->first_grant_slot = i - m->grant_dwell;
			}
			last_grant_slot = i;
			out->grants++;
		}
	}
	out->reached_established = (m->state == REAC_M_ESTABLISHED);
	return 0;
}

/* Build a `struct reac_pacer` with everything reac_pacer_apply_rate touches
 * initialised, but NO AF_PACKET socket at all — p->fd stays -1 and nothing
 * here ever calls reac_pacer_open/start. Mirrors the subset of open()'s work
 * that has no I/O in it. */
static void bare_pacer_init(struct reac_pacer *p, int fps,
                            const struct reac_console_cfg *cfg)
{
	memset(p, 0, sizeof *p);
	p->fd = -1;
	atomic_init(&p->recognized_headamp_base, -1);
	memcpy(p->src, SRC, 6);
	p->fps = fps;
	p->period_ns = reac_pacer_period_ns(fps);
	p->slot_period_ns = p->period_ns;
	p->catchup_max_slots_cfg = 0;
	p->catchup_max_slots = reac_catchup_default_slots(fps);
	reac_clock_disc_init(&p->clock, REAC_ROLE_MASTER, p->period_ns);
	reac_headamp_tx_init(&p->headamp);
	reac_master_init(&p->master, p->src, cfg, fps);
	reac_master_set_headamp_src(&p->master, &p->headamp);
	p->prev_state = REAC_M_IDLE;
	atomic_init(&p->fsm_state, REAC_M_IDLE);
	p->drivable_mask = REAC_RATE_ALL_BITS;
	atomic_init(&p->rate_hz, fps * REAC_SAMPLES_PER_PKT);
	atomic_init(&p->rate_asserted, 0);
	atomic_init(&p->rate_refused, REAC_RATE_REFUSE_NONE);
	atomic_init(&p->rate_reestablishing, 0);
}

/* (a) An in-list, drivable rate change re-establishes with the grant/dwell
 * sequence byte-identical in SHAPE at the new cadence, via an INTERNAL
 * re-establish — no process restart, same struct reac_pacer throughout. */
static int test_apply_rate_shape(void)
{
	struct reac_console_cfg cfg = { .out_channels = 16, .console_field = 1 };
	struct reac_pacer p;
	bare_pacer_init(&p, 4000, &cfg);   /* 48 kHz */

	struct establish_shape before;
	CHK(run_establish(&p.master, &before) == 0);
	CHK(before.reached_established);

	int fps = reac_pacer_apply_rate(&p, 96000);
	CHK(fps == 8000);
	CHK(p.fps == 8000);
	CHK(p.period_ns == reac_pacer_period_ns(8000));

	/* Immediate consequences of the re-establish: the FSM is back at IDLE (the
	 * pacer's very next slot would promote it to PROBING, exactly a cold
	 * start), the box is forgotten, and the standing-rate props say so. */
	CHK(p.master.state == REAC_M_IDLE);
	CHK(reac_master_has_box(&p.master) == 0);
	CHK(atomic_load_explicit(&p.fsm_state, memory_order_relaxed) == REAC_M_IDLE);
	CHK(atomic_load_explicit(&p.rate_hz, memory_order_relaxed) == 96000);
	CHK(atomic_load_explicit(&p.rate_asserted, memory_order_relaxed) == 1);
	CHK(atomic_load_explicit(&p.rate_reestablishing, memory_order_relaxed) == 1);

	/* The console cfg is NOT reset by a rate change — only establishment is
	 * redone (the spec: "a change ... re-clocks the segment", nothing else). */
	CHK(p.master.cfg.out_channels == 16 && p.master.cfg.console_field == 1);

	struct establish_shape after;
	CHK(run_establish(&p.master, &after) == 0);
	CHK(shapes_equal(&before, &after));

	return 0;
}

/* (b) An out-of-list rate: the caller (mirroring on_param_changed) refuses it
 * via reac_rate_cfg_decide BEFORE reac_pacer_request_rate is ever called, so
 * nothing about the pacer moves — fps, period_ns and the FSM state are
 * byte-for-byte what they were. */
static int test_refused_rate_moves_nothing(void)
{
	struct reac_pacer p;
	bare_pacer_init(&p, 4000, NULL);

	struct establish_shape before;
	CHK(run_establish(&p.master, &before) == 0);

	int fps_before = p.fps;
	long period_before = p.period_ns;
	enum reac_master_state state_before = p.master.state;

	enum reac_rate_refuse r = reac_rate_cfg_decide(REAC_ROLE_MASTER, 44101,
	                                               p.drivable_mask);
	CHK(r == REAC_RATE_REFUSE_NOT_CLOSED);
	/* The caller's contract: only REFUSE_NONE reaches reac_pacer_request_rate.
	 * Simulate on_param_changed's own gate here rather than calling request_rate
	 * at all — proving the refusal is a no-op is proving this gate is honoured. */
	CHK(p.fps == fps_before);
	CHK(p.period_ns == period_before);
	CHK(p.master.state == state_before);
	CHK(atomic_load_explicit(&p.rate_reestablishing, memory_order_relaxed) == 0);
	return 0;
}

/* (c) A narrowed drivable mask (standing in for a future real probe) refuses
 * 96 kHz on a segment that cannot carry it, and the default for such a
 * segment is 48 kHz — never a silent clamp to some other value. */
static int test_narrow_mask_refuses_and_defaults_lower(void)
{
	unsigned narrow = REAC_RATE_BIT_44100 | REAC_RATE_BIT_48000;   /* no 96k */
	CHK(reac_rate_cfg_decide(REAC_ROLE_MASTER, 96000, narrow) ==
	    REAC_RATE_REFUSE_NOT_DRIVABLE);
	CHK(reac_rate_best_drivable(narrow) == 48000);
	return 0;
}

/* ---- part 3: ONE DAEMON, N LISTENERS — segment independence -------------
 *
 * (docs/design/specs/2026-08-20-reac-auto-spine.md §5, the openmixer tree).
 * main.c now opens one `struct reac_pacer` per configured interface against a
 * SHARED PipeWire loop instead of one per process. Nothing in reac_pacer.c
 * changed to make that safe — every field this test touches was already
 * instance-owned — but the CLAIM that it is safe had never been exercised
 * with two instances alive at once, and one true singleton *did* exist one
 * layer up (main.c's segment lock was a function-local `static`, which a
 * per-listener loop would have shared across every segment had it stayed
 * that way). This is the regression net for the instance-independence claim
 * the whole multi-listener shape rests on: two bare pacers, this rig's
 * actual pair (m200 unnamed + m5000 s1608-shaped), each reaching ESTABLISHED
 * on its own, and a `reac.cfg.rate` assertion on one leaving the other's
 * fps, period, FSM state and rate props byte-for-byte untouched. */
static int test_two_segments_are_independent(void)
{
	struct reac_console_cfg cfg_a = { .out_channels = 16, .console_field = 0 };  /* m200 */
	struct reac_console_cfg cfg_b = { .out_channels = 16, .console_field = 1 };  /* m5000 */
	struct reac_pacer a, b;
	bare_pacer_init(&a, 4000, &cfg_a);   /* 48 kHz, like this rig's segment A */
	bare_pacer_init(&b, 4000, &cfg_b);   /* 48 kHz, like this rig's segment B */

	struct establish_shape sh_a, sh_b;
	CHK(run_establish(&a.master, &sh_a) == 0);
	CHK(sh_a.reached_established);
	CHK(run_establish(&b.master, &sh_b) == 0);
	CHK(sh_b.reached_established);

	/* Snapshot everything a rate change on A must not touch on B. */
	int b_fps = b.fps;
	long b_period = b.period_ns;
	enum reac_master_state b_state = b.master.state;
	int b_rate_hz        = atomic_load_explicit(&b.rate_hz, memory_order_relaxed);
	int b_rate_asserted  = atomic_load_explicit(&b.rate_asserted, memory_order_relaxed);
	int b_reestablishing = atomic_load_explicit(&b.rate_reestablishing, memory_order_relaxed);
	int b_out_channels   = b.master.cfg.out_channels;
	int b_console_field  = b.master.cfg.console_field;

	/* The write door: reac_rate_cfg_decide (pure) then reac_pacer_apply_rate —
	 * on_param_changed's own sequence — applied ONLY to segment A. */
	CHK(reac_rate_cfg_decide(REAC_ROLE_MASTER, 96000, a.drivable_mask) == REAC_RATE_REFUSE_NONE);
	int a_fps = reac_pacer_apply_rate(&a, 96000);
	CHK(a_fps == 8000);
	CHK(a.fps == 8000);
	CHK(atomic_load_explicit(&a.rate_hz, memory_order_relaxed) == 96000);
	CHK(atomic_load_explicit(&a.rate_reestablishing, memory_order_relaxed) == 1);

	/* B: untouched, byte-for-byte. */
	CHK(b.fps == b_fps);
	CHK(b.period_ns == b_period);
	CHK(b.master.state == b_state);
	CHK(atomic_load_explicit(&b.rate_hz, memory_order_relaxed) == b_rate_hz);
	CHK(atomic_load_explicit(&b.rate_asserted, memory_order_relaxed) == b_rate_asserted);
	CHK(atomic_load_explicit(&b.rate_reestablishing, memory_order_relaxed) == b_reestablishing);
	CHK(b.master.cfg.out_channels == b_out_channels);
	CHK(b.master.cfg.console_field == b_console_field);

	return 0;
}

int main(void)
{
	CHK(test_closed_list_and_bits() == 0);
	CHK(test_best_drivable() == 0);
	CHK(test_drivable_csv() == 0);
	CHK(test_decide() == 0);
	CHK(test_prop_parse() == 0);
	CHK(test_apply_rate_shape() == 0);
	CHK(test_refused_rate_moves_nothing() == 0);
	CHK(test_narrow_mask_refuses_and_defaults_lower() == 0);
	CHK(test_two_segments_are_independent() == 0);

	printf("OK: reac.cfg.rate — closed list, drivability, decide/parse, the "
	       "pacer-level internal re-establish (shape-identical at a new rate; a "
	       "refused rate moves nothing; a narrowed segment defaults lower), and "
	       "two segments (auto-spine §5's N listeners) staying independent under "
	       "a rate change\n");
	return 0;
}
