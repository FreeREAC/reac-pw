// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* THE WIRE MODEL: a silent wire is knocked on, never flooded (#40, #31, #39).
 *
 * A fake wire counts every frame the daemon puts on it, by kind (libreac's reac_ctrl_parse),
 * and notes when each knock opened. On it sit, case by case, nothing at all; a linked silent
 * box; a trunk parent whose VLANs carry REAC. The daemon's side is what main.c runs for an
 * unpinned wire: libreac's hunt fed by every frame heard, reac_knock_wire stepped every
 * 200 ms poll and run when its timer falls due, and the master stream started when the hunt
 * says MASTER. The stream is modelled as the pacer is: libreac's reac_master stepped every
 * slot and every slot sent, FILLER included, with the answer the knock kept handed to it
 * first (main.c does that with reac_pacer_rx_ingest before the pacer thread starts).
 *
 * THE BUDGETS ARE THE WIRE'S, NEVER THE MODULE'S. What a knock may carry is derived here from
 * libreac's own master, stepped through one hunting cycle from the header (cycle_ctrl, the
 * frames it stamps with a control block; cycle_slots, the whole cycle), and what went out is
 * counted on the fake wire. No counter of reac_knock_wire's is asserted as a budget.
 *
 * TWO BOXES. `fake_box` is libreac's, rule for rule (tools/fake_box.c, from the desk-arrival
 * capture of 2026-09-14): silent until a COMPLETE transfer (FIRST, 341 MIDDLE, LAST) reaches
 * it, an interrupted one ignored; it answers 8.4 ms after the LAST with its declaration, then
 * streams upstream filler. It ignores FILLER by construction, so it answers a control knock,
 * as both of the rig's box families did on 2026-10-27 (docs/KNOCK.md). `stream_box` is the
 * box no record shows: one that answers a transfer only if the master's FILLER ran through
 * it. The knock never sends a FILLER (#61), so that box is never found by it, and this file
 * states the trade rather than hiding it.
 *
 * WHAT GOES RED ON THE OLD PROBE. Before #40 the licence was handed to the hunt
 * (reac_hunt_silence_proven) and the stream went out at every slot of a wire nothing had been
 * heard on. Put that line back into reac_knock_wire_step and A, B and C fail on their stream
 * counts. Make reac_knock_wire_period_ns return one cycle and A fails on the gaps; let the
 * knock send its FILLER slots and A, B2 and H fail on their FILLER counts.
 *
 *   A. A SILENT WIRE, ten minutes: ZERO master-stream frames and ZERO FILLER; knock gaps of
 *      1, 2, 4, 8 cycles, then the 30 s ceiling; at most one control cycle per 30 s on the
 *      wire once the ceiling is reached.
 *   B. A DIRECT NIC WITH fake_box: the first answer ends the knock, the stream starts only
 *      after it, and the master that starts reaches ENROLL.
 *   B2. A DIRECT NIC WITH stream_box: never answered in a minute, and not one FILLER sent
 *      for it: the trade, stated.
 *   C. A TRUNK PARENT: knocks untagged until tagged REAC is heard on its VLANs, then sends
 *      nothing, and the tag-stripped box frames its socket then hears neither answer a knock
 *      nor make it a segment.
 *   D. A PINNED WIRE IS NEVER KNOCKED: a pin serves on link.
 *   E. A BOX POWERED ON A WIRE BEING KNOCKED ON: its flood of FILLER does not end the knock,
 *      so it hears a master and JOINs; the JOIN ends the knock and is what reaches the
 *      master, which leaves PROBING on it.
 *   F. THE RESET: a wire backed off to the ceiling hears one frame after a silence; the next
 *      knock opens at once and the gaps start again from one cycle.
 *   G. THE KNOCK'S WAKE LADDER, with values: the edge falls on the first once-a-second step
 *      past the 12 s grace with three whole pushes behind it, across the knock's rests; a
 *      wire that carried any frame is refused the edge; a wire that is the host's NIC is
 *      refused it by name (#46).
 *   H. THE HOST'S NIC (#46): the same silent ten minutes as A on a NIC the kernel says is the
 *      host's: ZERO FILLER frames, the same control budget and the same gaps, the knock being
 *      the same on either; and stream_box on that NIC is never answered either.
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
#define MAX_OPENS 64

static const uint8_t OUR_MAC[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t BOX_MAC[6] = { 0x00, 0x40, 0xab, 0xc4, 0x08, 0xbc };  /* fake_box.c */

/* ONE HUNTING CYCLE OF libreac's MASTER, from the header: the frames it stamps with a control
 * block and the slots in all. This is the knock's content by definition, derived here from
 * libreac and never typed in. */
static int cycle_ctrl, cycle_slots, transfer_slots;
static uint64_t cycle_ns;

static void derive_cycle(int fps)
{
	struct reac_master m;
	struct reac_console_cfg c = REAC_CONSOLE_CFG_IDLE;
	c.console_field = reac_pace_code(fps);
	reac_master_init(&m, OUR_MAC, &c, fps);
	for (int i = 0; i < m.cycle_len && m.cycle_pos != m.sub01_off; i++) {
		uint16_t counter;
		int idx;
		(void)reac_master_next(&m, &counter, &idx);
	}
	cycle_slots = m.cycle_len;
	cycle_ctrl = 0;
	for (int i = 0; i < m.cycle_len; i++) {
		uint16_t counter;
		int idx;
		const enum reac_master_emit e = reac_master_next(&m, &counter, &idx);
		if (e != REAC_M_EMIT_FILLER)
			cycle_ctrl++;
		if (e == REAC_M_EMIT_SCENE_TAIL)
			transfer_slots = i;     /* header to LAST */
	}
	cycle_ns = (uint64_t)cycle_slots * SEC / (uint64_t)fps;
}

struct wire {
	uint64_t now;
	/* every frame the daemon put on the wire */
	uint64_t frames, filler, first, middle, last, enroll;
	uint64_t knock_frames;          /* of which the knock's */
	uint64_t stream_frames;         /* of which the master stream's */
	uint64_t first_stream_ns;       /* when the stream's first frame went out (0 = never) */
	uint64_t frames_since;          /* knock frames from `since_ns` on */
	uint64_t since_ns;
	int sending_stream;             /* who is sending this frame */
	/* when each knock opened: the time of each knock FIRST */
	uint64_t open_ns[MAX_OPENS];
	int n_open;
	/* the box */
	int box;
	int needs_filler;               /* stream_box: a transfer counts only through FILLER */
	int saw_first, mids, filler_in;
	int committed;
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
		if (w->now >= w->since_ns)
			w->frames_since++;
	}
	if (k == REAC_CTRL_FILLER) {
		w->filler++;
		if (w->saw_first)
			w->filler_in++;
	}
	if (k == REAC_CTRL_SCENE_TRANSFER && p.link == REAC_LINK_CTRL && p.opcode == REAC_OP_BULK) {
		if (p.seg == REAC_SEG_FIRST) {
			w->first++;
			w->saw_first = 1;
			w->mids = 0;
			w->filler_in = 0;
			/* a knock opens on its transfer's header and carries one */
			if (!w->sending_stream && w->n_open < MAX_OPENS)
				w->open_ns[w->n_open++] = w->now;
		} else if (p.seg == REAC_SEG_MIDDLE) {
			w->middle++;
			if (w->saw_first)
				w->mids++;
		} else if (p.seg == REAC_SEG_LAST) {
			w->last++;
			/* fake_box.c: a complete transfer, and the box has not committed yet */
			if (w->box && w->saw_first && w->mids == REAC_SCENE_CHUNKS && !w->committed &&
			    !w->commit_at && (!w->needs_filler || w->filler_in > 0))
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
	int handed;                     /* control frames handed to the master at serve */
	int granting_seen;              /* the master left PROBING forward at some slot */
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
		const int seen = reac_hunt_observe(&d->hunt, w->rx[i], w->rx_len[i], w->now, &sight);
		/* main.c's evidence: a sighting whose settled role is a box or a master */
		if (seen >= 0 && !d->served)
			apply(d, reac_knock_wire_heard(&d->kw, w->now, w->rx[i], w->rx_len[i],
			                               sight.role != REAC_DISCO_ROLE_UNKNOWN), w->now);
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
	d->handed = d->kw.n_answer;
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
	/* main.c's timer: armed at the knock's due time, a rest included */
	if (d->knock_open && !d->served && d->kw.due_ns && w->now >= d->kw.due_ns) {
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
			if (d->eng.state == REAC_M_GRANTING || d->eng.state == REAC_M_ESTABLISHED)
				d->granting_seen = 1;
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

/* |a - b| <= tol */
static int near(uint64_t a, uint64_t b, uint64_t tol)
{
	return a > b ? a - b <= tol : b - a <= tol;
}

/* ---- A. A SILENT WIRE, TEN MINUTES ---------------------------------------------------- */
static int arm_silent_wire(uint64_t t0)
{
	struct daemon d;
	static struct wire w;
	/* Ten minutes on a silent wire is control knocks and NOT ONE FILLER: the bar every wire
	 * is held to, the trunk's native VLAN among them (#61). */
	memset(&w, 0, sizeof w);
	daemon_init(&d, t0, 0);
	w.since_ns = t0 + 300 * SEC;
	run(&d, &w, t0, 600, 0, 0);
	fprintf(stderr, "A: 600 s on a silent wire: %llu frames (%llu knock, %llu stream, %llu "
	        "FILLER) in %d knocks; a cycle is %d slots, %d of them control (libreac's master)\n",
	        (unsigned long long)w.frames, (unsigned long long)w.knock_frames,
	        (unsigned long long)w.stream_frames, (unsigned long long)w.filler, w.n_open,
	        cycle_slots, cycle_ctrl);
	fprintf(stderr, "A: knock gaps (s):");
	for (int i = 1; i < w.n_open; i++)
		fprintf(stderr, " %.3f", (double)(w.open_ns[i] - w.open_ns[i - 1]) / 1e9);
	fprintf(stderr, "\n");
	CHK(w.filler == 0);
	CHK(w.stream_frames == 0);
	CHK(!d.served);
	CHK(d.acts_knock == 1);
	/* nothing before the masterless observation closed */
	CHK(w.n_open > 0 && w.open_ns[0] >= t0 + REAC_KNOCK_LISTEN_NS);
	/* every knock is one cycle's control frames and nothing more */
	CHK(w.frames <= (uint64_t)w.n_open * (uint64_t)cycle_ctrl);
	CHK(w.frames >= (uint64_t)w.n_open * (uint64_t)(cycle_ctrl - 1));
	/* THE BACKOFF, with values: 1, 2, 4, 8 cycles, then the ceiling for good */
	CHK(w.n_open >= 10);
	for (int i = 1; i < w.n_open; i++) {
		uint64_t want = cycle_ns << (i - 1);
		if (want > REAC_KNOCK_WIRE_PERIOD_MAX_NS || i - 1 >= 16)
			want = REAC_KNOCK_WIRE_PERIOD_MAX_NS;
		CHK(near(w.open_ns[i] - w.open_ns[i - 1], want, 2 * MS));
	}
	/* AND AT THE CEILING, the second five minutes carry at most one knock per ceiling
	 * period: 10 or 11 of them, each at most one cycle's control frames */
	CHK(w.frames_since <= 11ULL * (uint64_t)cycle_ctrl);
	CHK(w.frames_since >= 9ULL * (uint64_t)(cycle_ctrl - 1));
	/* POSITIVE CONTROL: every knock is the sequence fake_box answers, a whole transfer */
	CHK(w.first == (uint64_t)w.n_open);
	CHK(w.middle == (uint64_t)w.n_open * REAC_SCENE_CHUNKS);
	/* and the module's own derivation agrees with this one */
	CHK(reac_knock_wire_control_per_cycle(FPS_96K) == cycle_ctrl);
	CHK(reac_knock_wire_cycle_slots(FPS_96K) == cycle_slots);
	return 0;
}

/* ---- B. A DIRECT NIC WITH fake_box ------------------------------------------------ */
static int arm_direct_nic(uint64_t t0)
{
	struct daemon d;
	static struct wire w;
	memset(&w, 0, sizeof w);
	daemon_init(&d, t0, 0);
	run(&d, &w, t0, 12, 0, t0);
	fprintf(stderr, "B: fake_box answered at +%.3f s on knock %llu; knock %llu frames, "
	        "stream %llu frames from +%.3f s; ENROLL %llu\n",
	        d.answered_ns ? (double)(d.answered_ns - t0) / 1e9 : -1.0,
	        (unsigned long long)d.kw.answered_cycle,
	        (unsigned long long)w.knock_frames, (unsigned long long)w.stream_frames,
	        w.first_stream_ns ? (double)(w.first_stream_ns - t0) / 1e9 : -1.0,
	        (unsigned long long)w.enroll);
	/* the stream starts only after a box answered, and the knock is over when it does */
	CHK(d.answered_ns > 0 && w.first_stream_ns >= d.answered_ns);
	CHK(d.served);
	CHK(d.acts_knock == 1);
	CHK(d.acts_hush == 1);
	CHK(d.kw.state != REAC_KNOCK_WIRE_KNOCKING);
	/* fake_box ignores FILLER, so the first control knock is enough for it */
	CHK(d.kw.answered_cycle == 1);
	CHK(w.knock_frames <= (uint64_t)cycle_ctrl);
	/* and the master that starts on the answer goes on to ENROLL the box */
	CHK(w.enroll > 0);
	return 0;
}

/* ---- B2. A DIRECT NIC WITH A BOX THAT ANSWERS ONLY THE STREAM ------------------------- */
static int arm_stream_box(uint64_t t0)
{
	struct daemon d;
	static struct wire w;
	/* THE KNOCK NEVER SENDS A FILLER (#61): a box that answers only the stream is not found
	 * by it, by design. The rig's boxes answer the control knock, and a stream on a wire
	 * nothing has answered on is a flood of it, the trunk's native VLAN first of all. */
	memset(&w, 0, sizeof w);
	w.needs_filler = 1;
	daemon_init(&d, t0, 0);
	run(&d, &w, t0, 60, 0, t0);
	fprintf(stderr, "B2: stream_box: %d knocks, %llu FILLER, answered=%d\n",
	        w.n_open, (unsigned long long)w.filler, d.acts_hush);
	CHK(w.filler == 0);
	CHK(w.stream_frames == 0);
	CHK(d.acts_hush == 0 && !d.served);
	CHK(w.n_open >= 5);
	return 0;
}

/* ---- C. A TRUNK PARENT ---------------------------------------------------------- */
static int arm_trunk_parent(uint64_t t0)
{
	struct daemon d;
	static struct wire w, w2;
	memset(&w, 0, sizeof w);
	daemon_init(&d, t0, 0);
	/* knocks untagged from +0.5 s; tagged REAC heard on a VLAN at +2 s; from +4 s its
	 * socket hears a box's frames with the tag stripped */
	run(&d, &w, t0, 2, 0, 0);
	const uint64_t before = w.knock_frames;
	CHK(w.stream_frames == 0 && !d.served);   /* the parent is knocked on, not streamed at */
	CHK(d.acts_knock == 1 && before > 0);
	w2 = w;
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
	static struct wire w;
	memset(&w, 0, sizeof w);
	daemon_init(&d, t0, 1);
	run(&d, &w, t0, 3, 0, 0);
	CHK(d.acts_knock == 0);
	CHK(w.knock_frames == 0);
	return 0;
}

/* ---- E. A BOX THAT COMES UP ON A WIRE BEING KNOCKED ON. libreac's own slave FSM, rule for
 * rule (reac_fsm.c FSM_FLOOD_ANNOUNCE): on PHY-up it floods broadcast FILLER, and it leaves
 * the flood for the cold-connect JOIN only once its bounded burst (REAC_FSM_FLOOD_BURST,
 * ~1.36 s) is spent AND it has heard a master. Its flood is FILLER of unknown role; if that
 * ended the knock, no master frame would reach it and it would flood for ever while we
 * listened. The JOIN is what lets a master leave PROBING, so it must be what is handed on. */
#define FLOOD_NS (1360ULL * MS)
static int arm_box_powers_up(uint64_t t0)
{
	static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	struct daemon d;
	static struct wire w;
	memset(&w, 0, sizeof w);
	daemon_init(&d, t0, 0);
	run(&d, &w, t0, 4, 0, 0);               /* knocking, nothing on the wire */
	CHK(d.acts_knock == 1 && !d.served);
	const uint64_t knock0 = w.knock_frames, frames0 = w.frames;
	uint16_t c = 0;
	int joins = 0;
	uint64_t join_ns = 0;
	for (uint64_t t = t0 + 4 * SEC; t < t0 + 14 * SEC; t += MS) {
		w.now = t;
		const uint64_t dt = t - (t0 + 4 * SEC);
		const int heard_master = w.frames > frames0;   /* any frame of ours: knock or stream */
		if (dt < FLOOD_NS || (!heard_master && !joins)) {   /* the flood: FILLER */
			for (int i = 0; i < 4; i++)
				w.rx_len[i] = reac_ctrl_build_flood_filler(w.rx[i], BCAST, BOX_MAC, c++, 16,
				                                           NULL, REAC_SAMPLES_PER_PKT);
			w.n_rx = 4;
		} else if (joins < 3) {                              /* then the JOIN, three times */
			w.rx_len[0] = reac_ctrl_build_coldconnect(w.rx[0], BCAST, BOX_MAC, c++, 16, NULL,
			                                          REAC_SAMPLES_PER_PKT);
			w.n_rx = 1;
			if (!joins)
				join_ns = t;
			joins++;
		}
		daemon_tick(&d, &w, (t - t0) % POLL_NS == 0);
	}
	fprintf(stderr, "E: box powered at +4 s: %llu knock frames reached it during its flood, "
	        "JOIN at +%.3f s, served=%d, %d control frame(s) handed over, the master %s "
	        "GRANTING\n", (unsigned long long)(w.knock_frames - knock0),
	        join_ns ? (double)(join_ns - t0) / 1e9 : -1.0, d.served, d.handed,
	        d.granting_seen ? "reached" : "never reached");
	CHK(joins > 0);                          /* the box heard a master */
	/* AND THE STREAM WAITED FOR THE EVIDENCE: the knock went on through the flood of
	 * unknown-role FILLER, and the master stream started only after the JOIN. */
	CHK(w.knock_frames > knock0);
	CHK(w.first_stream_ns >= join_ns);
	CHK(d.acts_hush == 1);
	CHK(d.served);
	CHK(d.handed > 0);
	/* the JOIN it was handed takes it out of PROBING once its own push is whole (libreac
	 * reac_master.c: a forward edge that lands mid-push is HELD, not dropped) */
	CHK(d.granting_seen);
	return 0;
}

/* ---- F. THE RESET ----------------------------------------------------------------- */
static int arm_reset(uint64_t t0)
{
	static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	struct daemon d;
	static struct wire w;
	memset(&w, 0, sizeof w);
	daemon_init(&d, t0, 0);
	run(&d, &w, t0, 200, 0, 0);              /* backed off to the ceiling */
	const int opens0 = w.n_open;
	CHK(opens0 >= 2);
	CHK(near(w.open_ns[opens0 - 1] - w.open_ns[opens0 - 2], REAC_KNOCK_WIRE_PERIOD_MAX_NS, 2 * MS));
	/* one frame that is not evidence, after a silence, 5 s into a 30 s rest. Handed to the
	 * knock alone: a box's flood frame would also take the hunt to MASTER (a box is present),
	 * which is arm E's case, and this arm is about the backoff. */
	const uint64_t heard = w.open_ns[opens0 - 1] + 5 * SEC;
	run(&d, &w, t0 + 200 * SEC, (heard - (t0 + 200 * SEC)) / SEC, 0, 0);
	w.now = heard;
	uint8_t f[REAC_FRAME_BYTES];
	const size_t fl = reac_ctrl_build_flood_filler(f, BCAST, BOX_MAC, 1, 16, NULL,
	                                               REAC_SAMPLES_PER_PKT);
	CHK(reac_knock_wire_heard(&d.kw, heard, f, fl, 0) == REAC_KNOCK_WIRE_ACT_NONE);
	daemon_tick(&d, &w, 0);
	run(&d, &w, heard + MS, 40, 0, 0);
	fprintf(stderr, "F: a frame at +%.3f s after a silence: next knock +%.3f s after it, then "
	        "gaps %.3f %.3f s\n", (double)(heard - t0) / 1e9,
	        (double)(w.open_ns[opens0] - heard) / 1e9,
	        (double)(w.open_ns[opens0 + 1] - w.open_ns[opens0]) / 1e9,
	        (double)(w.open_ns[opens0 + 2] - w.open_ns[opens0 + 1]) / 1e9);
	CHK(w.n_open >= opens0 + 3);
	CHK(w.open_ns[opens0] - heard <= 2 * MS);                       /* at once */
	CHK(near(w.open_ns[opens0 + 1] - w.open_ns[opens0], cycle_ns, 2 * MS));      /* from 1 */
	CHK(near(w.open_ns[opens0 + 2] - w.open_ns[opens0 + 1], 2 * cycle_ns, 2 * MS));
	return 0;
}

/* ---- G. THE KNOCK'S WAKE LADDER, once a second as main.c steps it ----------------------- */
static int arm_ladder(uint64_t t0, uint64_t rx_per_s, int host, enum reac_wake_act want,
                      enum reac_wake_refusal refusal)
{
	struct daemon d;
	static struct wire w;
	struct reac_knock_ladder kl;
	memset(&w, 0, sizeof w);
	memset(&kl, 0, sizeof kl);
	daemon_init(&d, t0, 0);
	uint64_t ladder_open = 0, bounced = 0, pushes_at = 0;
	enum reac_wake_refusal last = REAC_WAKE_OK;
	for (uint64_t t = t0; t < t0 + 40 * SEC && !bounced; t += MS) {
		w.now = t;
		daemon_tick(&d, &w, (t - t0) % POLL_NS == 0);
		if ((t - t0) % SEC || d.kw.state != REAC_KNOCK_WIRE_KNOCKING)
			continue;
		struct reac_wake_obs o;
		const enum reac_wake_act a =
			reac_knock_ladder_step(&kl, &d.kw, t, rx_per_s * ((t - t0) / SEC), 1, 0, host, &o);
		if (!ladder_open)
			ladder_open = t;
		last = kl.wake.refusal;
		if (a == REAC_WAKE_ACT_BOUNCE) {
			bounced = t;
			pushes_at = o.scene_pushes;
		}
		/* across the rests the frames are counted from the spell: never NOTHING_SENT */
		CHK(kl.wake.refusal != REAC_WAKE_NOTHING_SENT);
	}
	fprintf(stderr, "G: %llu box frames a second%s: ladder opened +%.3f s, %s at +%.3f s with %llu pushes "
	        "(last refusal %d)\n", (unsigned long long)rx_per_s, host ? ", the host's NIC" : "",
	        (double)(ladder_open - t0) / 1e9, bounced ? "EDGE" : "no edge",
	        bounced ? (double)(bounced - t0) / 1e9 : -1.0, (unsigned long long)pushes_at,
	        (int)last);
	if (want == REAC_WAKE_ACT_BOUNCE) {
		/* the first step at or past the grace, with three whole pushes behind it */
		CHK(bounced == ladder_open + REAC_WAKE_GRACE_NS);
		CHK(pushes_at >= REAC_WAKE_MIN_PUSHES);
	} else {
		CHK(!bounced);
		CHK(last == refusal);
	}
	return 0;
}

/* ---- H. THE HOST'S NIC: no FILLER ever, the control budget and the gaps unchanged ------ */
static int arm_host_nic(uint64_t t0)
{
	struct daemon d;
	static struct wire w;
	memset(&w, 0, sizeof w);
	daemon_init(&d, t0, 0);
	reac_knock_wire_set_host(&d.kw, 1);
	w.since_ns = t0 + 300 * SEC;
	run(&d, &w, t0, 600, 0, 0);
	fprintf(stderr, "H: 600 s on a silent wire that is the host's NIC: %llu frames (%llu knock, "
	        "%llu stream, %llu FILLER) in %d knocks\n",
	        (unsigned long long)w.frames, (unsigned long long)w.knock_frames,
	        (unsigned long long)w.stream_frames, (unsigned long long)w.filler, w.n_open);
	CHK(w.stream_frames == 0);
	CHK(!d.served);
	/* NOT ONE FILLER on the host's NIC: the knock is the same control-only knock as A's */
	CHK(w.filler == 0);
	CHK(d.kw.host == 1);
	/* the same backoff as A, so the host's NIC is knocked on no more often */
	CHK(w.n_open >= 10);
	for (int i = 1; i < w.n_open; i++) {
		uint64_t want = cycle_ns << (i - 1);
		if (want > REAC_KNOCK_WIRE_PERIOD_MAX_NS || i - 1 >= 16)
			want = REAC_KNOCK_WIRE_PERIOD_MAX_NS;
		CHK(near(w.open_ns[i] - w.open_ns[i - 1], want, 2 * MS));
	}
	/* every knock is one cycle's control frames and nothing more */
	CHK(w.frames <= (uint64_t)w.n_open * (uint64_t)cycle_ctrl);
	CHK(w.frames >= (uint64_t)w.n_open * (uint64_t)(cycle_ctrl - 1));
	CHK(w.first == (uint64_t)w.n_open);
	CHK(w.middle == (uint64_t)w.n_open * REAC_SCENE_CHUNKS);
	/* THE TRADE, stated here too: a box that answers only the stream is not found on the
	 * host's NIC, as it is not found on any wire (B2) */
	memset(&w, 0, sizeof w);
	w.needs_filler = 1;
	daemon_init(&d, t0, 0);
	reac_knock_wire_set_host(&d.kw, 1);
	run(&d, &w, t0, 60, 0, t0);
	fprintf(stderr, "H: stream_box on the host's NIC: %d knocks, %llu FILLER, answered=%d\n",
	        w.n_open, (unsigned long long)w.filler, d.acts_hush);
	CHK(w.filler == 0);
	CHK(d.acts_hush == 0 && !d.served);
	CHK(w.n_open >= 5);
	return 0;
}

int main(void)
{
	const uint64_t t0 = 1000ULL * SEC;
	derive_cycle(FPS_96K);
	fprintf(stderr, "libreac's hunting cycle: 48 kHz %d slots, %d control; 96 kHz %d slots, %d "
	        "control\n", reac_knock_wire_cycle_slots(4000), reac_knock_wire_control_per_cycle(4000),
	        reac_knock_wire_cycle_slots(FPS_96K), reac_knock_wire_control_per_cycle(FPS_96K));
	int failed = 0;
	failed += arm_silent_wire(t0);
	failed += arm_direct_nic(t0);
	failed += arm_stream_box(t0);
	failed += arm_trunk_parent(t0);
	failed += arm_pinned(t0);
	failed += arm_box_powers_up(t0);
	failed += arm_reset(t0);
	failed += arm_ladder(t0, 0, 0, REAC_WAKE_ACT_BOUNCE, REAC_WAKE_OK);
	failed += arm_ladder(t0, 5, 0, REAC_WAKE_ACT_NONE, REAC_WAKE_BOX_IS_TALKING);
	failed += arm_ladder(t0, 0, 1, REAC_WAKE_ACT_NONE, REAC_WAKE_HOST_NIC);
	failed += arm_host_nic(t0);
	if (failed) {
		fprintf(stderr, "FAIL: %d of 11 arms\n", failed);
		return 1;
	}
	printf("OK: a silent wire is knocked on with control frames only, inside its derived "
	       "budget, backs off to the ceiling and is never streamed at; the first answer ends "
	       "the knock and starts the stream; a trunk parent goes quiet; the ladder bounces on "
	       "schedule and never on the host's NIC\n");
	return 0;
}
