// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_knock_wire: see reac_knock_wire.h for what a silent wire is sent and why.

#include "reac_knock_wire.h"

#include <string.h>
#include <strings.h>

#include <reac/reac_ctrlblk.h>  /* reac_ctrl_parse: which answer frames are kept */
#include <reac/reac_encode.h>   /* reac_downstream_build */

/* ---- THE PLANS. The one place a knock's content is decided; docs/KNOCK.md has why. ---- */
static const struct reac_knock_rung plan_escalate[] = {
	{ REAC_KNOCK_CONTROL, 2 },  /* the bounded knock, unproven on a real box            */
	{ REAC_KNOCK_STREAM,  1 },  /* then one cycle of what a real box was seen to answer */
};
static const struct reac_knock_rung plan_control[] = {
	{ REAC_KNOCK_CONTROL, 1 },
};
static const struct reac_knock_rung plan_stream[] = {
	{ REAC_KNOCK_STREAM, 1 },
};

const struct reac_knock_plan reac_knock_plans[] = {
	{ "escalate", "two control knocks, then one stream cycle",
	  plan_escalate, (int)(sizeof plan_escalate / sizeof plan_escalate[0]) },
	{ "control", "control knocks only, never a FILLER",
	  plan_control, (int)(sizeof plan_control / sizeof plan_control[0]) },
	{ "stream", "one stream cycle on every knock",
	  plan_stream, (int)(sizeof plan_stream / sizeof plan_stream[0]) },
};
const int reac_knock_n_plans = (int)(sizeof reac_knock_plans / sizeof reac_knock_plans[0]);

const struct reac_knock_plan *reac_knock_plan_find(const char *name)
{
	if (!name)
		return NULL;
	for (int i = 0; i < reac_knock_n_plans; i++)
		if (strcasecmp(reac_knock_plans[i].name, name) == 0)
			return &reac_knock_plans[i];
	return NULL;
}

static void plan_reset(struct reac_knock_wire *w)
{
	w->rung = 0;
	w->rung_done = 0;
	w->unanswered = 0;
	w->stream_knocks = 0;
}

void reac_knock_wire_init(struct reac_knock_wire *w, const uint8_t src[6], int fps,
                          uint64_t now_ns)
{
	memset(w, 0, sizeof *w);
	memcpy(w->src, src, 6);
	w->fps = fps;
	w->plan = reac_knock_plan_find(REAC_KNOCK_PLAN_DEFAULT);
	w->state = REAC_KNOCK_WIRE_LISTENING;
	reac_knock_init(&w->licence, now_ns);
}

void reac_knock_wire_set_plan(struct reac_knock_wire *w, const struct reac_knock_plan *plan)
{
	w->plan = plan ? plan : reac_knock_plan_find(REAC_KNOCK_PLAN_DEFAULT);
	w->rung = 0;
	w->rung_done = 0;
}

void reac_knock_wire_set_host(struct reac_knock_wire *w, int host)
{
	w->host = host ? 1 : 0;
}

static uint64_t slot_ns(const struct reac_knock_wire *w, uint64_t slot)
{
	return w->t0_ns + slot * 1000000000ULL / (uint64_t)w->fps;
}

/* A FRESH MASTER, field for field the one the pacer would start: the idle console, the pace
 * code of this cadence, this NIC's own address (libreac reac_pacer.c reac_pacer_open). The
 * scene it pushes is the one the master that follows pushes, so the box commits the same body
 * twice and the second ENROLL finds it ready. Stepped, without a clock and without the wire,
 * to the transfer's header: a master's cycle starts with the chunk burst and puts the header
 * five slots before the NEXT cycle (reac_master.c: "Step 0 (the header) went out at sub01_off
 * five slots before this cycle opened"), so a knock that opened at slot 0 would carry a burst
 * with no FIRST, which a box ignores. */
static void master_at_header(struct reac_master *m, const uint8_t src[6], int fps)
{
	struct reac_console_cfg c = REAC_CONSOLE_CFG_IDLE;
	c.console_field = reac_pace_code(fps);
	reac_master_init(m, src, &c, fps);
	for (int i = 0; i < m->cycle_len && m->cycle_pos != m->sub01_off; i++) {
		uint16_t counter;
		int idx;
		(void)reac_master_next(m, &counter, &idx);
	}
}

int reac_knock_wire_cycle_slots(int fps)
{
	struct reac_master m;
	static const uint8_t src[6] = { 0x02, 0, 0, 0, 0, 0 };
	if (fps <= 0)
		return 0;
	master_at_header(&m, src, fps);
	return m.cycle_len;
}

int reac_knock_wire_control_per_cycle(int fps)
{
	struct reac_master m;
	static const uint8_t src[6] = { 0x02, 0, 0, 0, 0, 0 };
	if (fps <= 0)
		return 0;
	master_at_header(&m, src, fps);
	int n = 0;
	for (int i = 0; i < m.cycle_len; i++) {
		uint16_t counter;
		int idx;
		if (reac_master_next(&m, &counter, &idx) != REAC_M_EMIT_FILLER)
			n++;
	}
	return n;
}

uint64_t reac_knock_wire_period_ns(const struct reac_knock_wire *w, unsigned n)
{
	const int slots = w->master.cycle_len > 0 ? w->master.cycle_len
	                                          : reac_knock_wire_cycle_slots(w->fps);
	if (w->fps <= 0 || slots <= 0)
		return REAC_KNOCK_WIRE_PERIOD_MAX_NS;
	uint64_t p = (uint64_t)slots * 1000000000ULL / (uint64_t)w->fps;
	for (unsigned i = 0; i < n && p < REAC_KNOCK_WIRE_PERIOD_MAX_NS; i++)
		p *= 2;
	return p < REAC_KNOCK_WIRE_PERIOD_MAX_NS ? p : REAC_KNOCK_WIRE_PERIOD_MAX_NS;
}

/* WHEN THE NEXT FRAME OF THE KNOCK FALLS DUE, asked of a COPY of the master so the real one
 * is stepped exactly once per slot, as the pacer steps it. Bounded by the knock's own end:
 * past its last slot the next thing due is the next knock. */
static void knock_peek(struct reac_knock_wire *w)
{
	if (w->resting) {
		w->due_ns = w->next_ns;
		return;
	}
	if (w->content == REAC_KNOCK_STREAM) {
		w->due_ns = slot_ns(w, w->slot);
		return;
	}
	struct reac_master peek = w->master;
	const uint64_t end = (uint64_t)w->master.cycle_len;
	for (uint64_t s = w->slot; s < end; s++) {
		uint16_t counter;
		int idx;
		if (reac_master_next(&peek, &counter, &idx) != REAC_M_EMIT_FILLER) {
			w->due_ns = slot_ns(w, s);
			return;
		}
	}
	w->due_ns = slot_ns(w, end);   /* the knock's end, where run() schedules the next */
}

/* The next knock's content: the plan's rung, the stream past its cap sent as control. */
static enum reac_knock_content plan_take(struct reac_knock_wire *w)
{
	const struct reac_knock_plan *p = w->plan;
	enum reac_knock_content c = REAC_KNOCK_CONTROL;
	if (p && p->n_rung > 0) {
		if (w->rung >= p->n_rung)
			w->rung = 0;
		c = p->rung[w->rung].content;
		if (++w->rung_done >= p->rung[w->rung].knocks) {
			w->rung_done = 0;
			w->rung = (w->rung + 1) % p->n_rung;
		}
	}
	if (c == REAC_KNOCK_STREAM) {
		if (w->host) {
			/* THE HOST'S NIC IS NEVER STREAMED AT (#46): the rung goes out as the
			 * bounded knock, and the cap is left alone for a wire that becomes bare. */
			c = REAC_KNOCK_CONTROL;
			w->stream_withheld++;
		} else if (w->stream_knocks >= REAC_KNOCK_WIRE_STREAM_KNOCKS) {
			c = REAC_KNOCK_CONTROL;
		} else {
			w->stream_knocks++;
		}
	}
	return c;
}

static void knock_open(struct reac_knock_wire *w, uint64_t at_ns)
{
	master_at_header(&w->master, w->src, w->fps);
	w->content = plan_take(w);
	w->resting = 0;
	w->t0_ns = at_ns;
	w->slot = 0;
	w->head_sent = 0;
	w->chunks_sent = 0;
	w->cycles++;
	knock_peek(w);
}

/* The knock's last slot has gone by unanswered: rest until the next one. */
static void knock_rest(struct reac_knock_wire *w)
{
	w->next_ns = w->t0_ns + reac_knock_wire_period_ns(w, w->unanswered);
	w->unanswered++;
	w->resting = 1;
	knock_peek(w);
}

enum reac_knock_wire_act reac_knock_wire_step(struct reac_knock_wire *w,
                                              const struct reac_hunt *hunt,
                                              uint64_t now_ns, int trunk_now)
{
	if (trunk_now) {
		if (w->state == REAC_KNOCK_WIRE_TRUNK)
			return REAC_KNOCK_WIRE_ACT_NONE;
		w->state = REAC_KNOCK_WIRE_TRUNK;
		w->due_ns = 0;
		return REAC_KNOCK_WIRE_ACT_TRUNK;
	}
	if (w->state == REAC_KNOCK_WIRE_TRUNK) {
		/* THE TAGS AGED OUT (#98: the trunk verdict is about the cable as it is now). The
		 * wire is listened to afresh, from this instant, like a wire that just came up. */
		w->state = REAC_KNOCK_WIRE_LISTENING;
		reac_knock_init(&w->licence, now_ns);
		plan_reset(w);
		return REAC_KNOCK_WIRE_ACT_UNTRUNK;
	}
	if (hunt && hunt->pinned)
		return REAC_KNOCK_WIRE_ACT_NONE;
	if (w->state == REAC_KNOCK_WIRE_KNOCKING)
		return REAC_KNOCK_WIRE_ACT_NONE;
	/* THE LICENCE BUYS A KNOCK, AND NOTHING ELSE. It used to buy the wire: the hunt was told
	 * the silence was proven, it answered MASTER, and the stream went out at every slot onto
	 * a wire nothing had ever been heard on (#31). The hunt now reaches MASTER the one way
	 * that is evidence, a box heard on the wire, and that box is the knock's answer. */
	if (reac_knock_step(&w->licence, now_ns) == REAC_KNOCK_ACT_DRIVE) {
		w->state = REAC_KNOCK_WIRE_KNOCKING;
		w->knocks++;
		w->cycles = 0;
		w->spell_sent = 0;
		w->n_answer = 0;
		knock_open(w, now_ns);
		return REAC_KNOCK_WIRE_ACT_KNOCK;
	}
	return REAC_KNOCK_WIRE_ACT_NONE;
}

enum reac_knock_wire_act reac_knock_wire_heard(struct reac_knock_wire *w, uint64_t now_ns,
                                               const uint8_t *frame, size_t len,
                                               int evidence)
{
	if (w->state == REAC_KNOCK_WIRE_TRUNK)
		return REAC_KNOCK_WIRE_ACT_NONE;
	/* SOMETHING CAME UP after a silence: the plan and the backoff start again, and a knock
	 * that is resting opens now. Only the first frame after REAC_KNOCK_LISTEN_NS of nothing,
	 * so a source that never stops cannot hold the backoff at its floor. */
	const int after_silence = !w->last_heard_ns ||
	                          now_ns - w->last_heard_ns >= REAC_KNOCK_LISTEN_NS;
	w->last_heard_ns = now_ns;
	if (after_silence) {
		plan_reset(w);
		if (w->state == REAC_KNOCK_WIRE_KNOCKING && w->resting) {
			w->next_ns = now_ns;
			w->due_ns = now_ns;
		}
	}
	/* libreac's licence is cancelled by ANY frame and re-opens from it (reac_knock.h: the
	 * cancellation is not a latch), so a wire that answered and went quiet again is knocked
	 * on again, 500 ms after the last frame it carried. */
	reac_knock_heard(&w->licence, now_ns);
	enum reac_knock_wire_act act = REAC_KNOCK_WIRE_ACT_NONE;
	if (w->state == REAC_KNOCK_WIRE_KNOCKING && evidence) {
		w->state = REAC_KNOCK_WIRE_ANSWERED;
		w->due_ns = 0;
		w->n_answer = 0;
		w->answered_cycle = w->cycles;
		w->answered_content = w->content;
		if (frame && len >= 12)
			memcpy(w->answer_src, frame + 6, 6);
		act = REAC_KNOCK_WIRE_ACT_HUSH;
	}
	/* WHAT IS KEPT IS WHAT THE MASTER ACTS ON: the box's control frames (its declaration
	 * after a whole transfer, its cold-connect JOIN), and not its audio or presence FILLER,
	 * which only says it is there (libreac reac_master.h: presence "never gates the
	 * grant"). Once a box streams to us, the frames after its answer are FILLER. */
	if (w->state == REAC_KNOCK_WIRE_ANSWERED && frame && len > 0 &&
	    w->n_answer < REAC_KNOCK_WIRE_ANSWERS &&
	    reac_ctrl_parse(frame, len, NULL) > REAC_CTRL_FILLER) {
		size_t n = len < REAC_FRAME_BYTES ? len : REAC_FRAME_BYTES;
		memcpy(w->answer[w->n_answer], frame, n);
		w->answer_len[w->n_answer] = n;
		w->n_answer++;
	}
	return act;
}

void reac_knock_wire_answer_taken(struct reac_knock_wire *w)
{
	w->n_answer = 0;
	if (w->state == REAC_KNOCK_WIRE_ANSWERED)
		w->state = REAC_KNOCK_WIRE_LISTENING;
}

int reac_knock_wire_run(struct reac_knock_wire *w, uint64_t now_ns,
                        reac_knock_wire_send_fn send, void *ctx)
{
	if (w->state != REAC_KNOCK_WIRE_KNOCKING || w->fps <= 0)
		return 0;
	int sent = 0;
	uint8_t frame[REAC_FRAME_BYTES];
	for (;;) {
		if (w->resting) {
			if (now_ns < w->next_ns)
				break;
			knock_open(w, w->next_ns);
		}
		if (w->slot >= (uint64_t)w->master.cycle_len) {
			knock_rest(w);
			continue;
		}
		const uint64_t at = slot_ns(w, w->slot);
		if (at > now_ns)
			break;
		uint16_t counter;
		int idx;
		enum reac_master_emit emit = reac_master_next(&w->master, &counter, &idx);
		w->slot++;
		/* THE FILLER IS THE STREAM: a control knock leaves it out, a stream knock sends it. */
		if (emit == REAC_M_EMIT_FILLER && w->content == REAC_KNOCK_CONTROL)
			continue;
		const int scene = emit == REAC_M_EMIT_SCENE_HEAD || emit == REAC_M_EMIT_SCENE_CHUNK ||
		                  emit == REAC_M_EMIT_SCENE_TAIL;
		int left = 0;
		if (now_ns - at > REAC_KNOCK_WIRE_LATE_NS) {
			w->late++;
		} else {
			/* The pacer's own two steps for a slot: a silent downstream frame with this
			 * slot's counter, then the master's control block over it (none on a FILLER). */
			reac_downstream_build(frame, NULL, 0, 0, counter, w->src);
			if (reac_master_stamp(&w->master, frame, emit, idx) == 0) {
				if (send(ctx, frame, sizeof frame) == 0)
					left = 1;
				else
					w->send_errors++;
			}
		}
		if (left) {
			w->sent++;
			w->spell_sent++;
			sent++;
			if (w->content == REAC_KNOCK_STREAM)
				w->stream_sent++;
		}
		if (!scene)
			continue;
		/* A TRANSFER IS COMPLETE ONLY IF ALL OF IT LEFT: the FIRST, every MIDDLE, the
		 * LAST. A box ignores a transfer with a hole in it, so one is not counted. */
		if (!left)
			w->head_sent = 0;
		else if (emit == REAC_M_EMIT_SCENE_HEAD) {
			w->head_sent = 1;
			w->chunks_sent = 0;
		} else if (emit == REAC_M_EMIT_SCENE_CHUNK) {
			w->chunks_sent++;
		} else if (w->head_sent && w->chunks_sent == REAC_SCENE_CHUNKS) {
			w->pushes++;
			w->head_sent = 0;
		}
	}
	knock_peek(w);
	return sent;
}

enum reac_wake_act reac_knock_ladder_step(struct reac_knock_ladder *kl,
                                          const struct reac_knock_wire *w, uint64_t now_ns,
                                          uint64_t rx_frames, int carrier,
                                          int siblings_served, int host_nic,
                                          struct reac_wake_obs *o)
{
	if (!kl->wake.opened_ns) {
		reac_wake_init(&kl->wake, now_ns);
	} else if (w->knocks != kl->knocks_seen || w->pushes < kl->pushes_last ||
	           rx_frames < kl->frames0) {
		/* A FRESH SPELL, after an answer that went quiet, or the fresh sniffer of a link
		 * that came back: the grace restarts from here and the EDGE COUNT does not. */
		reac_wake_reopen(&kl->wake, now_ns);
	} else {
		goto observe;
	}
	kl->pushes0 = w->pushes;
	kl->knocks_seen = w->knocks;
	kl->frames0 = rx_frames;
observe:
	kl->pushes_last = w->pushes;
	memset(o, 0, sizeof *o);
	o->probing = 1;
	o->carrier = carrier;
	/* A BOX THAT FLOODS IS TALKING, and an edge would only cost it its flood: any 0x8819
	 * frame this wire carried since the spell opened refuses the bounce. Evidence would
	 * have ended the knock; this is the unknown-role rest. */
	o->rx_box_frames = rx_frames - kl->frames0;
	o->scene_pushes = w->pushes - kl->pushes0;
	o->siblings_served = siblings_served;
	o->host_nic = host_nic;
	o->tx_frames = w->spell_sent;
	return reac_wake_step(&kl->wake, now_ns, o);
}

const char *reac_knock_wire_state_name(enum reac_knock_wire_state s)
{
	switch (s) {
	case REAC_KNOCK_WIRE_LISTENING: return "listening";
	case REAC_KNOCK_WIRE_KNOCKING:  return "knocking";
	case REAC_KNOCK_WIRE_ANSWERED:  return "answered";
	case REAC_KNOCK_WIRE_TRUNK:     return "trunk";
	}
	return "listening";
}

const char *reac_knock_content_name(enum reac_knock_content c)
{
	switch (c) {
	case REAC_KNOCK_CONTROL: return "control";
	case REAC_KNOCK_STREAM:  return "stream";
	}
	return "control";
}
