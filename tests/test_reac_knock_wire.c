// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* THE WIRE MODEL: a silent wire is knocked on, never flooded (#40, #31, #39).
 *
 * A fake wire counts every frame the daemon puts on it, by kind (libreac's reac_ctrl_parse).
 * On it sit, case by case, nothing at all; a linked silent box; a trunk parent whose VLANs
 * carry REAC. The daemon's side is what main.c's 200 ms poll runs for an unpinned wire:
 * libreac's hunt fed by every frame heard, reac_knock_wire stepped and run, and the master
 * stream started when the hunt says MASTER. The stream is modelled as the pacer is: libreac's
 * reac_master stepped every slot and every slot sent, FILLER included, with the answer the
 * knock kept handed to it first (main.c does that with reac_pacer_rx_ingest before the pacer
 * thread starts).
 *
 * THE BOX IS libreac's fake_box, RULE FOR RULE (tools/fake_box.c, from the desk-arrival
 * capture of 2026-09-14): silent until a COMPLETE transfer (FIRST, 341 MIDDLE, LAST) reaches
 * it, an interrupted one ignored; it answers 8.4 ms after the LAST with its declaration,
 * unicast to whoever pushed, then streams upstream filler to it. The frames are libreac's
 * builders, not bytes typed here.
 *
 * WHAT GOES RED ON THE OLD PROBE. Before #40 the licence was handed to the hunt
 * (reac_hunt_silence_proven) and the stream went out at every slot of a wire nothing had been
 * heard on. Put that line back into reac_knock_wire_step and A, B and C fail on their stream
 * counts; that is the sabotage recorded in the PR.
 *
 *   A. A SILENT WIRE: at most REAC_KNOCK_WIRE_BUDGET_PER_CYCLE frames per cycle, ZERO stream
 *      frames, and every cycle carries a whole transfer (the knock is the sequence a box
 *      answers, not a token).
 *   B. A DIRECT NIC WITH A COLD BOX: the first frame heard ends the knock, no knock frame
 *      after it, the stream starts only after it, and the master that starts reaches ENROLL
 *      (the box's next step, its JOIN, follows the group map: fake_box.c).
 *   C. A TRUNK PARENT: knocks untagged until tagged REAC is heard on its VLANs, then sends
 *      nothing, and the tag-stripped box frames its socket then hears neither answer a knock
 *      nor make it a segment.
 *   D. A PINNED WIRE IS NEVER KNOCKED: a pin serves on link.
 */
#include "reac_knock_wire.h"

#include <reac/reac_ctrl.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_encode.h>
#include <reac/reac_hunt.h>
#include <reac/reac_master.h>

#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

#define MS  1000000ULL
#define SEC 1000000000ULL
#define FPS_96K 8000
#define POLL_NS (200ULL * MS)
#define COMMIT_DELAY_NS (8400ULL * 1000ULL)   /* fake_box.c COMMIT_DELAY_NS */
#define BOX_STREAM_NS   (2ULL * MS)

static const uint8_t OUR_MAC[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t BOX_MAC[6] = { 0x00, 0x40, 0xab, 0xc4, 0x08, 0xbc };  /* fake_box.c */

struct wire {
	uint64_t now;
	/* every frame the daemon put on the wire */
	uint64_t frames, filler, first, middle, last, enroll;
	uint64_t knock_frames;          /* of which the knock's */
	uint64_t stream_frames;         /* of which the master stream's */
	uint64_t first_stream_ns;       /* when the stream's first frame went out (0 = never) */
	int sending_stream;             /* who is sending this frame */
	/* the box */
	int box;
	int saw_first, mids, committed;
	uint64_t commit_at, next_stream;
	uint16_t box_counter;
	/* the box's frames, delivered to the daemon's socket */
	uint8_t rx[4][REAC_FRAME_BYTES];
	size_t rx_len[4];
	int n_rx;
};

static int wire_send(void *ctx, const uint8_t *f, size_t n)
{
	struct wire *w = ctx;
	struct reac_ctrl_parsed p;
	enum reac_ctrl_kind k = reac_ctrl_parse(f, n, &p);
	w->frames++;
	if (w->sending_stream) {
		w->stream_frames++;
		if (!w->first_stream_ns)
			w->first_stream_ns = w->now;
	} else {
		w->knock_frames++;
	}
	if (k == REAC_CTRL_FILLER)
		w->filler++;
	if (k == REAC_CTRL_SCENE_TRANSFER && p.link == REAC_LINK_CTRL && p.opcode == REAC_OP_BULK) {
		if (p.seg == REAC_SEG_FIRST) {
			w->first++;
			w->saw_first = 1;
			w->mids = 0;
		} else if (p.seg == REAC_SEG_MIDDLE) {
			w->middle++;
			if (w->saw_first)
				w->mids++;
		} else if (p.seg == REAC_SEG_LAST) {
			w->last++;
			/* fake_box.c: a complete transfer, and the box has not committed yet */
			if (w->box && w->saw_first && w->mids == REAC_SCENE_CHUNKS && !w->committed &&
			    !w->commit_at)
				w->commit_at = w->now + COMMIT_DELAY_NS;
			w->saw_first = 0;
			w->mids = 0;
		}
	}
	/* the frame fake_box JOINs on, 1.489 s later (fake_box.c: `k == REAC_CTRL_GROUP_MAP`) */
	if (k == REAC_CTRL_GROUP_MAP)
		w->enroll++;
	return 0;
}

/* The box's turn: what it sends now, onto the daemon's socket. */
static void box_tick(struct wire *w)
{
	if (!w->box)
		return;
	if (w->commit_at && w->now >= w->commit_at && w->n_rx < 4) {
		w->rx_len[w->n_rx] = reac_ctrl_build_config_announce(w->rx[w->n_rx], OUR_MAC, BOX_MAC,
		                                                     w->box_counter++, 32);
		w->n_rx++;
		w->committed = 1;
		w->commit_at = 0;
		w->next_stream = w->now + BOX_STREAM_NS;
	}
	if (w->committed && w->now >= w->next_stream && w->n_rx < 4) {
		w->rx_len[w->n_rx] = reac_ctrl_build_upstream_filler(w->rx[w->n_rx], OUR_MAC, BOX_MAC,
		                                                     w->box_counter++, 8, NULL,
		                                                     REAC_SAMPLES_PER_PKT);
		w->n_rx++;
		w->next_stream = w->now + BOX_STREAM_NS;
	}
}

/* THE DAEMON'S SIDE, as main.c's poll runs it for one unpinned wire. */
struct daemon {
	struct reac_hunt hunt;
	struct reac_knock_wire kw;
	int knock_open;                 /* the sender is open */
	unsigned acts_knock, acts_hush, acts_trunk;
	int trunk_now;
	int served;                     /* the master stream is running */
	struct reac_master eng;
	uint64_t eng_t0, eng_slot;
	uint64_t answered_ns;
};

static void daemon_init(struct daemon *d, uint64_t t0, int pinned)
{
	memset(d, 0, sizeof *d);
	reac_hunt_init(&d->hunt, OUR_MAC, t0);
	if (pinned)
		reac_hunt_pin(&d->hunt, REAC_ROLE_MASTER);
	reac_knock_wire_init(&d->kw, OUR_MAC, FPS_96K, t0);
}

static void apply(struct daemon *d, enum reac_knock_wire_act a, uint64_t now)
{
	switch (a) {
	case REAC_KNOCK_WIRE_ACT_KNOCK: d->knock_open = 1; d->acts_knock++; break;
	case REAC_KNOCK_WIRE_ACT_HUSH:  d->knock_open = 0; d->acts_hush++; d->answered_ns = now; break;
	case REAC_KNOCK_WIRE_ACT_TRUNK: d->knock_open = 0; d->acts_trunk++; break;
	default: break;
	}
}

/* The sniffer's callback: every frame the box sent reaches the hunt and the knock. */
static void daemon_hear(struct daemon *d, struct wire *w)
{
	for (int i = 0; i < w->n_rx; i++) {
		struct reac_disco_sighting sight;
		if (reac_hunt_observe(&d->hunt, w->rx[i], w->rx_len[i], w->now, &sight) >= 0 &&
		    !d->served)
			apply(d, reac_knock_wire_heard(&d->kw, w->now, w->rx[i], w->rx_len[i]), w->now);
		/* and the running master's own RX, which is how the pacer hears its box */
		if (d->served) {
			struct reac_ctrl_parsed p;
			enum reac_master_rx_event ev;
			if (reac_ctrl_classify_box_frame(w->rx[i], w->rx_len[i], OUR_MAC, &p, &ev) == 0)
				reac_master_rx(&d->eng, ev, p.src, w->rx[i] + REAC_CTRL_BLOCK_OFF);
		}
	}
	w->n_rx = 0;
}

/* hearing_serve for a MASTER verdict: the pacer's master, the kept answer ingested first. */
static void daemon_serve(struct daemon *d, uint64_t now)
{
	struct reac_console_cfg c = REAC_CONSOLE_CFG_IDLE;
	c.console_field = reac_pace_code(FPS_96K);
	reac_master_init(&d->eng, OUR_MAC, &c, FPS_96K);
	for (int i = 0; i < d->kw.n_answer; i++) {
		struct reac_ctrl_parsed p;
		enum reac_master_rx_event ev;
		if (reac_ctrl_classify_box_frame(d->kw.answer[i], d->kw.answer_len[i], OUR_MAC,
		                                 &p, &ev) == 0)
			reac_master_rx(&d->eng, ev, p.src, d->kw.answer[i] + REAC_CTRL_BLOCK_OFF);
	}
	reac_knock_wire_answer_taken(&d->kw);
	d->eng_t0 = now;
	d->eng_slot = 0;
	d->served = 1;
}

static void daemon_tick(struct daemon *d, struct wire *w, int poll)
{
	daemon_hear(d, w);
	if (poll && !d->served) {
		apply(d, reac_knock_wire_step(&d->kw, &d->hunt, w->now, d->trunk_now), w->now);
		reac_hunt_step(&d->hunt, w->now);
		/* A TRUNK PARENT IS NOT A SEGMENT: main.c skips the hunt's verdict on it. */
		if (!d->trunk_now && d->hunt.verdict == REAC_HUNT_MASTER)
			daemon_serve(d, w->now);
	}
	if (d->knock_open && !d->served) {
		w->sending_stream = 0;
		reac_knock_wire_run(&d->kw, w->now, wire_send, w);
	}
	if (d->served) {
		w->sending_stream = 1;
		uint8_t frame[REAC_FRAME_BYTES];
		while (d->eng_t0 + d->eng_slot * SEC / FPS_96K <= w->now) {
			uint16_t counter;
			int idx;
			enum reac_master_emit e = reac_master_next(&d->eng, &counter, &idx);
			reac_downstream_build(frame, NULL, 0, 0, counter, OUR_MAC);
			reac_master_stamp(&d->eng, frame, e, idx);
			wire_send(w, frame, sizeof frame);
			d->eng_slot++;
		}
	}
}

/* Run the model from t0 to t0 + secs, one millisecond at a time. */
static void run(struct daemon *d, struct wire *w, uint64_t t0, uint64_t secs,
                uint64_t trunk_at, uint64_t box_at)
{
	for (uint64_t t = t0; t < t0 + secs * SEC; t += MS) {
		w->now = t;
		if (trunk_at && t >= trunk_at)
			d->trunk_now = 1;
		if (box_at && t >= box_at)
			w->box = 1;
		box_tick(w);
		daemon_tick(d, w, (t - t0) % POLL_NS == 0);
	}
}

/* ---- A. A SILENT WIRE ------------------------------------------------------------ */
static int arm_silent_wire(uint64_t t0)
{
	struct daemon d;
	struct wire w;
	memset(&w, 0, sizeof w);
	daemon_init(&d, t0, 0);
	run(&d, &w, t0, 30, 0, 0);
	fprintf(stderr, "A: 30 s on a silent wire: %llu frames (%llu knock, %llu stream, "
	        "%llu FILLER), worst cycle %llu (budget %d), %llu complete transfers\n",
	        (unsigned long long)w.frames, (unsigned long long)w.knock_frames,
	        (unsigned long long)w.stream_frames, (unsigned long long)w.filler,
	        (unsigned long long)d.kw.cycle_max, REAC_KNOCK_WIRE_BUDGET_PER_CYCLE,
	        (unsigned long long)d.kw.pushes);
	CHK(w.stream_frames == 0);
	CHK(w.filler == 0);
	CHK(!d.served);
	CHK(d.kw.cycle_max <= REAC_KNOCK_WIRE_BUDGET_PER_CYCLE);
	/* the stream's own count over the same 30 s, for scale: 240 000 frames */
	CHK(w.frames < 30 * FPS_96K / 50);
	/* POSITIVE CONTROL: the knock is the sequence a box answers, every cycle. */
	CHK(d.acts_knock == 1);
	CHK(d.kw.pushes >= 10);
	CHK(w.first == d.kw.pushes || w.first == d.kw.pushes + 1);
	CHK(w.middle >= d.kw.pushes * REAC_SCENE_CHUNKS);
	/* nothing before the masterless observation closed */
	CHK(d.kw.t0_ns >= t0 + REAC_KNOCK_LISTEN_NS);
	return 0;
}

/* ---- B. A DIRECT NIC WITH A COLD BOX ------------------------------------------- */
static int arm_direct_nic(uint64_t t0)
{
	struct daemon d;
	struct wire w;
	memset(&w, 0, sizeof w);
	daemon_init(&d, t0, 0);
	run(&d, &w, t0, 12, 0, t0);
	fprintf(stderr, "B: box answered at +%.3f s; knock %llu frames, stream %llu frames "
	        "from +%.3f s; ENROLL %llu\n",
	        d.answered_ns ? (double)(d.answered_ns - t0) / 1e9 : -1.0,
	        (unsigned long long)w.knock_frames, (unsigned long long)w.stream_frames,
	        w.first_stream_ns ? (double)(w.first_stream_ns - t0) / 1e9 : -1.0,
	        (unsigned long long)w.enroll);
	/* the stream starts only after a box answered, and the knock is over when it does */
	CHK(d.answered_ns > 0 && w.first_stream_ns >= d.answered_ns);
	CHK(d.served);
	CHK(d.acts_knock == 1);
	CHK(d.acts_hush == 1);
	CHK(d.kw.state != REAC_KNOCK_WIRE_KNOCKING);
	CHK(w.knock_frames <= 2 * REAC_KNOCK_WIRE_BUDGET_PER_CYCLE);
	/* and the master that starts on the answer goes on to ENROLL the box */
	CHK(w.enroll > 0);
	return 0;
}

/* ---- C. A TRUNK PARENT ---------------------------------------------------------- */
static int arm_trunk_parent(uint64_t t0)
{
	struct daemon d;
	struct wire w;
	memset(&w, 0, sizeof w);
	daemon_init(&d, t0, 0);
	/* knocks untagged from +0.5 s; tagged REAC heard on a VLAN at +2 s; from +4 s its
	 * socket hears a box's frames with the tag stripped */
	run(&d, &w, t0, 2, 0, 0);
	const uint64_t before = w.knock_frames;
	CHK(w.stream_frames == 0 && !d.served);   /* the parent is knocked on, not streamed at */
	CHK(d.acts_knock == 1 && before > 0);
	struct wire w2 = w;
	run(&d, &w2, t0 + 2 * SEC, 2, t0 + 2 * SEC, 0);
	CHK(d.acts_trunk == 1);
	CHK(w2.frames == w.frames);           /* nothing at all after the tags */
	for (int i = 0; i < 4; i++) {
		w2.rx_len[i] = reac_ctrl_build_upstream_filler(w2.rx[i], OUR_MAC, BOX_MAC,
		                                               (uint16_t)i, 8, NULL,
		                                               REAC_SAMPLES_PER_PKT);
	}
	w2.n_rx = 4;
	run(&d, &w2, t0 + 4 * SEC, 10, t0, 0);
	fprintf(stderr, "C: trunk parent: %llu knock frames before the tags, %llu after; "
	        "served=%d state=%s\n", (unsigned long long)before,
	        (unsigned long long)(w2.frames - w.frames), d.served,
	        reac_knock_wire_state_name(d.kw.state));
	CHK(w2.frames == w.frames);
	CHK(!d.served);
	CHK(d.kw.state == REAC_KNOCK_WIRE_TRUNK);
	CHK(d.acts_hush == 0);
	CHK(d.acts_trunk == 1);               /* said once */
	return 0;
}

/* ---- D. A PINNED WIRE IS NEVER KNOCKED ----------------------------------------- */
static int arm_pinned(uint64_t t0)
{
	struct daemon d;
	struct wire w;
	memset(&w, 0, sizeof w);
	daemon_init(&d, t0, 1);
	run(&d, &w, t0, 3, 0, 0);
	CHK(d.acts_knock == 0);
	CHK(w.knock_frames == 0);
	return 0;
}

int main(void)
{
	const uint64_t t0 = 1000ULL * SEC;
	int failed = 0;
	failed += arm_silent_wire(t0);
	failed += arm_direct_nic(t0);
	failed += arm_trunk_parent(t0);
	failed += arm_pinned(t0);
	if (failed) {
		fprintf(stderr, "FAIL: %d of 4 arms\n", failed);
		return 1;
	}
	printf("OK: a silent wire is knocked on within its budget and never streamed at; the "
	       "first answer ends the knock and starts the stream; a trunk parent goes quiet\n");
	return 0;
}
