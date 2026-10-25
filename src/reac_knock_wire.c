// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_knock_wire: see reac_knock_wire.h for what a silent wire is sent and why.

#include "reac_knock_wire.h"

#include <string.h>

#include <reac/reac_ctrlblk.h>  /* reac_ctrl_parse: which answer frames are kept */
#include <reac/reac_encode.h>   /* reac_downstream_build */

void reac_knock_wire_init(struct reac_knock_wire *w, const uint8_t src[6], int fps,
                          uint64_t now_ns)
{
	memset(w, 0, sizeof *w);
	memcpy(w->src, src, 6);
	w->fps = fps;
	w->state = REAC_KNOCK_WIRE_LISTENING;
	reac_knock_init(&w->licence, now_ns);
}

static uint64_t slot_ns(const struct reac_knock_wire *w, uint64_t slot)
{
	return w->t0_ns + slot * 1000000000ULL / (uint64_t)w->fps;
}

/* WHEN THE NEXT FRAME OF THE KNOCK FALLS DUE, asked of a COPY of the master so the real one
 * is stepped exactly once per slot, as the pacer steps it. Bounded by one cycle: every cycle
 * carries an announce, so the walk always ends inside it. */
static void knock_peek(struct reac_knock_wire *w)
{
	struct reac_master peek = w->master;
	int bound = w->master.cycle_len > 0 ? w->master.cycle_len : w->fps * 3;
	for (int i = 0; i < bound; i++) {
		uint16_t counter;
		int idx;
		if (reac_master_next(&peek, &counter, &idx) != REAC_M_EMIT_FILLER) {
			w->due_ns = slot_ns(w, w->slot + (uint64_t)i);
			return;
		}
	}
	w->due_ns = slot_ns(w, w->slot + (uint64_t)bound);
}

static void knock_open(struct reac_knock_wire *w, uint64_t now_ns)
{
	/* THE MASTER THE PACER WOULD START, field for field: the idle console, the pace code
	 * of this cadence, this NIC's own address (libreac reac_pacer.c reac_pacer_open). The
	 * scene it pushes is the one the master that follows pushes, so the box commits the
	 * same body twice and the second ENROLL finds it ready. */
	struct reac_console_cfg c = REAC_CONSOLE_CFG_IDLE;
	c.console_field = reac_pace_code(w->fps);
	reac_master_init(&w->master, w->src, &c, w->fps);
	/* THE KNOCK OPENS ON THE TRANSFER'S HEADER. A master's cycle starts with the chunk
	 * burst and puts the header five slots before the NEXT cycle (reac_master.c: "Step 0
	 * (the header) went out at sub01_off five slots before this cycle opened"), so the first
	 * burst of a fresh master has no FIRST and a box ignores it. The pacer sends it anyway;
	 * the knock steps the master through those slots without a clock and without the wire,
	 * so its first frame is the header and its first transfer is whole. */
	for (int i = 0; i < w->master.cycle_len && w->master.cycle_pos != w->master.sub01_off;
	     i++) {
		uint16_t counter;
		int idx;
		(void)reac_master_next(&w->master, &counter, &idx);
	}
	w->t0_ns = now_ns;
	w->slot = 0;
	w->head_sent = 0;
	w->chunks_sent = 0;
	w->cycle = 0;
	w->cycle_sent = 0;
	w->n_answer = 0;
	w->knocks++;
	w->state = REAC_KNOCK_WIRE_KNOCKING;
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
	/* libreac's licence is cancelled by ANY frame and re-opens from it (reac_knock.h: the
	 * cancellation is not a latch), so a wire that answered and went quiet again is knocked
	 * on again, 500 ms after the last frame it carried. */
	reac_knock_heard(&w->licence, now_ns);
	enum reac_knock_wire_act act = REAC_KNOCK_WIRE_ACT_NONE;
	if (w->state == REAC_KNOCK_WIRE_KNOCKING && evidence) {
		w->state = REAC_KNOCK_WIRE_ANSWERED;
		w->due_ns = 0;
		w->n_answer = 0;
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
	while (slot_ns(w, w->slot) <= now_ns) {
		const uint64_t at = slot_ns(w, w->slot);
		const uint64_t cycle = w->master.cycle_len > 0
		                       ? w->slot / (uint64_t)w->master.cycle_len : 0;
		if (cycle != w->cycle) {
			w->cycle = cycle;
			w->cycle_sent = 0;
		}
		uint16_t counter;
		int idx;
		enum reac_master_emit emit = reac_master_next(&w->master, &counter, &idx);
		w->slot++;
		/* THE FILLER IS THE STREAM, AND THE STREAM IS WHAT A KNOCK IS NOT. */
		if (emit == REAC_M_EMIT_FILLER)
			continue;
		const int scene = emit == REAC_M_EMIT_SCENE_HEAD || emit == REAC_M_EMIT_SCENE_CHUNK ||
		                  emit == REAC_M_EMIT_SCENE_TAIL;
		int left = 0;
		if (now_ns - at > REAC_KNOCK_WIRE_LATE_NS) {
			w->late++;
		} else {
			/* The pacer's own two steps for a control slot: a silent downstream frame
			 * with this slot's counter, then the master's control block over it. */
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
			sent++;
			if (++w->cycle_sent > w->cycle_max)
				w->cycle_max = w->cycle_sent;
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
