// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* clock_nanosleep / TIMER_ABSTIME, sched_setscheduler */
#endif
#include "reac_pacer.h"
#include "reac_ctrl.h"     /* reac_ctrl_classify_box_frame */
#include "reac_mac.h"

#include <reac/reac.h>     /* REAC_FRAME_BYTES, REAC_HDR_COUNTER_OFF, ... */
#include <reac/reac_ports.h> /* the box's declared port table (config-announce) */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <arpa/inet.h>     /* htons */

/* ---- lock-free SPSC frame ring (whole encoded frames) -------------------- */

static uint32_t next_pow2(uint32_t v)
{
	if (v < 2)
		return 2;
	v--;
	v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
	return v + 1;
}

int reac_frame_ring_init(struct reac_frame_ring *r, uint32_t slots, uint32_t slot_sz)
{
	memset(r, 0, sizeof *r);
	uint32_t n = next_pow2(slots);
	r->buf = calloc((size_t)n * slot_sz, 1);
	r->len = calloc(n, sizeof *r->len);
	if (!r->buf || !r->len) {
		free(r->buf); free(r->len);
		r->buf = NULL; r->len = NULL;
		return -1;
	}
	r->slots = n;
	r->mask = n - 1;
	r->slot_sz = slot_sz;
	atomic_store_explicit(&r->head, 0, memory_order_relaxed);
	atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
	return 0;
}

void reac_frame_ring_free(struct reac_frame_ring *r)
{
	free(r->buf); free(r->len);
	r->buf = NULL; r->len = NULL;
}

uint32_t reac_frame_ring_readable(const struct reac_frame_ring *r)
{
	uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
	uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
	return (h - t) & r->mask;
}

int reac_frame_ring_push(struct reac_frame_ring *r, const uint8_t *frame, uint16_t n)
{
	if (n > r->slot_sz)
		n = (uint16_t)r->slot_sz;
	uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
	uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
	if (((h + 1) & r->mask) == (t & r->mask)) {  /* full: drop the newest */
		atomic_fetch_add_explicit(&r->overruns, 1, memory_order_relaxed);
		return 0;
	}
	uint32_t slot = h & r->mask;
	memcpy(r->buf + (size_t)slot * r->slot_sz, frame, n);
	r->len[slot] = n;
	atomic_store_explicit(&r->head, (h + 1) & r->mask, memory_order_release);
	return 1;
}

uint32_t reac_pacer_guard_high(uint32_t quantum_frames)
{
	uint32_t burst = REAC_PACER_GUARD_BURST_MULT * quantum_frames;
	return burst > REAC_PACER_GUARD_FLOOR_FRAMES ? burst
	                                             : REAC_PACER_GUARD_FLOOR_FRAMES;
}

uint32_t reac_frame_ring_trim_count(uint32_t depth, uint32_t high, uint32_t target)
{
	if (depth <= high)
		return 0;
	if (target >= depth)   /* defensive: never an underflowing "drop" */
		return 0;
	return depth - target;
}

uint32_t reac_frame_ring_trim(struct reac_frame_ring *r, uint32_t high, uint32_t target)
{
	uint32_t depth = reac_frame_ring_readable(r);
	uint32_t drop = reac_frame_ring_trim_count(depth, high, target);
	if (drop == 0)
		return 0;
	/* CONSUMER moves tail — legal in SPSC (mirrors reac_ring_trim). */
	uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
	atomic_store_explicit(&r->tail, (t + drop) & r->mask, memory_order_release);
	return drop;
}

uint16_t reac_frame_ring_pop(struct reac_frame_ring *r, uint8_t *out)
{
	uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
	uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
	if ((t & r->mask) == (h & r->mask)) {        /* empty: underrun */
		atomic_fetch_add_explicit(&r->underruns, 1, memory_order_relaxed);
		return 0;
	}
	uint32_t slot = t & r->mask;
	uint16_t n = r->len[slot];
	memcpy(out, r->buf + (size_t)slot * r->slot_sz, n);
	atomic_store_explicit(&r->tail, (t + 1) & r->mask, memory_order_release);
	return n;
}

/* ---- timing ------------------------------------------------------------- */

long reac_pacer_period_ns(int fps)
{
	if (fps <= 0)
		fps = 8000;
	return (long)(1000000000.0 / (double)fps + 0.5);  /* 125000 @8000, 250000 @4000 */
}

/* CLOCK_MONOTONIC in ns. Exposed (reac_pacer.h) rather than file-static so the sink
 * node's property poll timestamps reac.discovery.* ages against the SAME clock the
 * pacer's sighting timestamps and staleness aging use — two clocks here would let a
 * device read as fresh in one place and withdrawn in the other. */
uint64_t reac_pacer_mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t mono_ns(void) { return reac_pacer_mono_ns(); }

/* Build a silent downstream FILLER (zero audio, zero control block) for an
 * underrun slot — keeps the cadence + counter + link alive when the graph has
 * nothing queued. The master control block is stamped afterwards by the caller. */
static void build_silent_filler(uint8_t *f, const uint8_t src[6])
{
	memset(f, 0, REAC_FRAME_BYTES);
	memset(f, 0xFF, 6);                    /* broadcast dst */
	memcpy(f + 6, src, 6);                 /* our master src */
	f[12] = (REAC_ETHERTYPE >> 8) & 0xFF;  /* 0x88 */
	f[13] = REAC_ETHERTYPE & 0xFF;         /* 0x19 */
	/* type 00 00 + zero control block; audio is silence (already zeroed). */
	f[REAC_FRAME_BYTES - 2] = REAC_END_MARKER_0;
	f[REAC_FRAME_BYTES - 1] = REAC_END_MARKER_1;
}

/* ---- the FSM event log ring (producer side: RT-safe, no I/O) ------------- */

/* Push one event; drop-newest on full (SPSC: the producer never moves tail). */
static void pev_push(struct reac_pacer *p, uint8_t kind, uint8_t a, uint8_t b,
                     const uint8_t src[6], const uint8_t blk[32])
{
	uint32_t h = atomic_load_explicit(&p->ev_head, memory_order_relaxed);
	uint32_t t = atomic_load_explicit(&p->ev_tail, memory_order_acquire);
	if (h - t >= REAC_PACER_EVRING) {
		atomic_fetch_add_explicit(&p->ev_drops, 1, memory_order_relaxed);
		return;
	}
	struct reac_pacer_event *e = &p->evring[h % REAC_PACER_EVRING];
	e->mono_ns = mono_ns();
	e->kind = kind; e->a = a; e->b = b;
	if (src) memcpy(e->src, src, 6); else memset(e->src, 0, 6);
	if (blk) memcpy(e->blk, blk, 32); else memset(e->blk, 0, 32);
	atomic_store_explicit(&p->ev_head, h + 1, memory_order_release);
}

/* A state transition happened (rx- or timer-driven): mirror it + log it.
 * Owns p->prev_state so an rx-driven transition is never re-logged by the
 * per-slot timer check. */
static void note_transition(struct reac_pacer *p, enum reac_master_state from,
                            enum reac_master_state to, uint8_t cause)
{
	p->prev_state = to;
	/* A RE-ESTABLISHMENT IS A NEW SESSION, and this is the one door every
	 * transition passes through — so the receiver is reset here, not by the next
	 * housekeeping tick. The tick was a whole tick late: the box's first frames
	 * of the new session arrived against the old session's counter and the seam
	 * was booked as lost frames (measured: 134 on a clean warm replug, where the
	 * correct answer is 0). Idempotent and allocation-free, so it is safe on this
	 * RT path; the epoch's release store publishes the new identity to the rx
	 * thread, which acquires it before using it. */
	if (p->on_session) {
		if (to == REAC_M_ESTABLISHED)
			p->on_session(p->session_ctx, p->master.box_mac,
			              p->master.session_seq);
		else if (from == REAC_M_ESTABLISHED)
			p->on_session(p->session_ctx, NULL, 0);   /* the session ENDED */
	}
	atomic_store_explicit(&p->fsm_state, to, memory_order_release);
	atomic_store_explicit(&p->grant_attempts, p->master.grant_attempts,
	                      memory_order_relaxed);
	memset(p->rx_since_change, 0, sizeof p->rx_since_change);
	p->probing_slots = 0;

	/* Every entry into ESTABLISHED replays the COMPLETE head-amp scene for the
	 * granted slots — the M-200's own answer to a box that power-cycled and came
	 * back with blank pins (measured: m200-s1608-BIDIR-reboot, the full width x 3
	 * scene 10 ms behind the re-grant). The grant-window sweep alone is not
	 * enough: a fast lock (box heartbeat accept) cuts it off mid-burst, and a
	 * partial scene mutes the unconfigured channels. Same thread as the table's
	 * emitter, so no synchronisation is needed. */
	if (to == REAC_M_ESTABLISHED && from != REAC_M_ESTABLISHED)
		reac_headamp_tx_arm_scene(&p->headamp, p->master.alloc.base,
		                          p->master.alloc.width);

	if (from == REAC_M_GRANTING && to == REAC_M_PROBING &&
	    p->master.drop_reason == REAC_M_DROP_GRANT_TIMEOUT) {
		atomic_fetch_add_explicit(&p->drops[REAC_M_DROP_GRANT_TIMEOUT], 1,
		                          memory_order_relaxed);
		pev_push(p, REAC_PEV_GRANT_TIMEOUT,
		         (uint8_t)(p->master.grant_attempts & 0xff), 0,
		         p->master.box_mac, NULL);
		return;
	}
	if (from == REAC_M_ESTABLISHED && to != REAC_M_ESTABLISHED) {
		enum reac_master_drop_reason r = p->master.drop_reason;
		atomic_fetch_add_explicit(&p->drops[r & 7], 1, memory_order_relaxed);
		pev_push(p, REAC_PEV_DROP, (uint8_t)r, (uint8_t)to, p->master.box_mac, NULL);
		return;
	}
	uint8_t blk[32] = { cause };
	pev_push(p, REAC_PEV_STATE, (uint8_t)from, (uint8_t)to, p->master.box_mac, blk);
}

/* The published box model is a MIRROR of the master's box identity, never a
 * parallel truth. The master forgets its box on every fall back to PROBING
 * (enter_probing -> reac_master_forget_box), so the published model has to go with
 * it — otherwise reac.box-model / reac.box-width keep naming a box that has left
 * the wire, and a consumer computing a head-amp address from that width addresses
 * a box that is not there. Call after every step of the master. */
static void sync_published_box(struct reac_pacer *p)
{
	if (!reac_master_has_box(&p->master)) {
		if (atomic_load_explicit(&p->recognized_box, memory_order_relaxed))
			atomic_store_explicit(&p->recognized_box, NULL, memory_order_release);
		/* Forget the declared geometry with the box, so a re-declaration after
		 * a drop re-fires set_box instead of deduping into silence. */
		p->declared_in = p->declared_out = 0;
	}
}

void reac_pacer_rx_ingest(struct reac_pacer *p, const uint8_t *frame, size_t len)
{
	/* PASSIVE DISCOVERY first, and independently (task #178). The socket is already
	 * promiscuous (see the PACKET_ADD_MEMBERSHIP rationale below), so every 0x8819
	 * frame on this segment arrives here — including the ones the master classifier
	 * is about to discard as none of its business: another master's broadcast
	 * (reac_ctrl.c:141) and unicast between third parties (reac_ctrl.c:144). Those
	 * discards ARE the discovery. Classify for sighting BEFORE the FSM filter, and
	 * with a separate ownership-blind classifier, so recording what is out there can
	 * never alter what the master does about it. */
	struct reac_disco_sighting sight;
	if (reac_disco_classify(frame, len, p->src, &sight) == 0 &&
	    reac_disco_gate_should_push(&p->disco_gate, &sight, mono_ns())) {
		/* The ring slot is bytes, not pointers: the model travels as its index in the
		 * fixed matrix, +1 so 0 reads as "unidentified". */
		int mi = reac_disco_model_index(sight.model);
		pev_push(p, REAC_PEV_SIGHTING, (uint8_t)sight.role,
		         (uint8_t)(mi + 1), sight.mac, NULL);
	}

	struct reac_ctrl_parsed parsed;
	enum reac_master_rx_event ev;
	if (reac_ctrl_classify_box_frame(frame, len, p->src, &parsed, &ev) != 0)
		return;

	atomic_fetch_add_explicit(&p->rx_box_frames, 1, memory_order_relaxed);
	if (parsed.kind != REAC_CTRL_FILLER)
		atomic_fetch_add_explicit(&p->rx_box_ctrl, 1, memory_order_relaxed);
	if (ev == REAC_M_RX_BOX_JOIN)
		atomic_fetch_add_explicit(&p->rx_joins, 1, memory_order_relaxed);

	/* MASTER as mixer: SELF-CONFIGURATION FROM THE WIRE. THE box geometry comes
	 * from what the box DECLARED — the config-announce port table (libreac
	 * reac_ports_parse) — never a hand-kept list. Everything downstream — the
	 * head-amp base, the grant sweep, the ENROLL group map, the cfea width, the
	 * published node props, the node widths — derives from set_box and from
	 * nothing else. The fixed matrix only NAMES the model for the log and the
	 * published props: an unnamed box is still sized and granted. Emit once per
	 * declared geometry (the box repeats its config-announce ~1/s). */
	struct reac_box_ports ports;
	if (len >= REAC_CTRL_BLOCK_OFF + REAC_CTRL_BLOCK_LEN &&
	    reac_ports_parse(frame + REAC_CTRL_BLOCK_OFF, &ports) == 0) {
		const struct reac_box_model *bm = reac_ctrl_identify_box(frame, len);
		const struct reac_box_model *prev_bm =
			atomic_load_explicit(&p->recognized_box, memory_order_relaxed);
		if (bm && bm != prev_bm)
			atomic_store_explicit(&p->recognized_box, bm, memory_order_release);
		if (ports.in_ch != p->declared_in || ports.out_ch != p->declared_out) {
			int prev_w = p->master.alloc.width;   /* the width ALREADY granted */
			reac_master_set_box(&p->master, ports.in_ch, ports.out_ch);
			/* set_box refuses a width it cannot place (it must not half-apply);
			 * record the declaration ONLY once it actually holds, so the box's
			 * next announce retries instead of being deduped into silence. */
			if (p->master.alloc.width == ports.in_ch) {
				/* set_box rebuilds the sweep for the declared width but does NOT
				 * re-emit it, and a box already GRANTED at a different width is
				 * holding an enrollment for slots that are not its own. Re-fire so
				 * it gets the corrected sweep (the S-4000S 8->32 fix, and the
				 * correct 8-wide base-0x00 grant for an S-0808 that had been
				 * granted 16 wide). A box declaring the width we already granted
				 * does NOT re-fire -> its establishment stays byte-identical.
				 *
				 * The `box_seen` term this gate also carried is gone. It is the
				 * 600-frame presence DECAY flag, and we are standing inside the
				 * handler for a frame from that very box — a "is the box there"
				 * test that can read 0 while we hold the box's own frame in our
				 * hands is a stale diagnostic gating a correctness action, which
				 * is how a wrong-width enrollment gets to survive on the wire. */
				if (ports.in_ch != prev_w &&
				    (p->master.state == REAC_M_GRANTING ||
				     p->master.state == REAC_M_ESTABLISHED))
					reac_master_regrant(&p->master);
				p->declared_in  = ports.in_ch;
				p->declared_out = ports.out_ch;
				pev_push(p, REAC_PEV_RECOGNIZED, (uint8_t)ports.in_ch,
				         (uint8_t)(reac_disco_model_index(bm) + 1),
				         parsed.src, NULL);
			}
		}
	}

	enum reac_master_state from = p->master.state;
	int changed = reac_master_rx(&p->master, ev, parsed.src,
	                             ev == REAC_M_RX_BOX_JOIN ? frame + 18 : NULL);
	sync_published_box(p);   /* a BYE / MAC-change drop forgets the box here */

	/* presence GAINED edge (LOST decays in the per-slot path) */
	if (p->master.box_seen && !p->presence_seen)
		pev_push(p, REAC_PEV_PRESENCE, 1, (uint8_t)ev, parsed.src, NULL);
	p->presence_seen = p->master.box_seen;

	/* JOIN and BYE always log with the full 32-byte block (the rig-capture
	 * substitute: if the real box's bytes differ from the zoneA template the
	 * drained log shows exactly what to update the matcher with). */
	if (ev == REAC_M_RX_BOX_JOIN) {
		pev_push(p, REAC_PEV_JOIN, parsed.is_broadcast ? 1 : 0,
		         (uint8_t)p->master.state, parsed.src, frame + 18);
	} else if (ev == REAC_M_RX_BOX_BYE) {
		pev_push(p, REAC_PEV_RX, (uint8_t)ev, (uint8_t)p->master.state,
		         parsed.src, frame + 18);
	} else {
		/* Rate-limited: the first frame of each kind after a state change in
		 * full; unicast HEARTBEATS thereafter 1-in-8; FILLER presence only on
		 * edges (above). Gate the 1-in-8 on the heartbeat KIND, not on
		 * BOX_UNICAST at large: the box's established upstream is audio FILLER
		 * unicast at wire rate (8000 fps @96k), which the classifier also maps
		 * to BOX_UNICAST — logging 1-in-8 of THAT floods the 128-slot ring
		 * (~1000 pev/s) and drop-newest silently evicts the JOIN/BYE/re-JOIN
		 * blocks the rig transcript exists to capture. For an established
		 * heartbeat, stash the latency since our last chanmap walk in blk[0..3]
		 * LE (healthy lockstep is 0.5-1.9 ms in the golden capture). */
		uint32_t seen = p->rx_since_change[ev & 3]++;
		int is_hb = (ev == REAC_M_RX_BOX_UNICAST &&
		             parsed.kind == REAC_CTRL_BOX_HB);
		int log_it = (seen == 0) || (is_hb && (seen & 7) == 0);
		if (log_it && ev != REAC_M_RX_BOX_BCAST_FILLER) {
			uint8_t blk[32] = { 0 };
			if (ev == REAC_M_RX_BOX_UNICAST && p->last_chanmap_ns) {
				uint32_t lat_us =
					(uint32_t)((mono_ns() - p->last_chanmap_ns) / 1000u);
				blk[0] = (uint8_t)lat_us; blk[1] = (uint8_t)(lat_us >> 8);
				blk[2] = (uint8_t)(lat_us >> 16); blk[3] = (uint8_t)(lat_us >> 24);
			}
			pev_push(p, REAC_PEV_RX, (uint8_t)ev, (uint8_t)p->master.state,
			         parsed.src, blk);
		}
	}

	if (changed)
		note_transition(p, from, p->master.state, (uint8_t)ev);
}

/* ---- the FSM event log (consumer side: any non-RT thread) ---------------- */

static void fmt_mac(char *out, const uint8_t m[6])
{
	sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void fmt_blk(char *out, const uint8_t blk[32])
{
	for (int i = 0; i < 32; i++)
		sprintf(out + 2 * i + (i / 8), "%02x%s", blk[i], ((i & 7) == 7) ? " " : "");
}

int reac_pacer_log_drain(struct reac_pacer *p, FILE *out)
{
	int count = 0;
	char mac[18], hex[80];
	uint32_t t = atomic_load_explicit(&p->ev_tail, memory_order_relaxed);
	for (;;) {
		uint32_t h = atomic_load_explicit(&p->ev_head, memory_order_acquire);
		if (t == h)
			break;
		struct reac_pacer_event e = p->evring[t % REAC_PACER_EVRING];
		atomic_store_explicit(&p->ev_tail, ++t, memory_order_release);
		count++;

		double ts = (double)e.mono_ns / 1e9;
		fmt_mac(mac, e.src);
		switch (e.kind) {
		case REAC_PEV_STATE:
			fprintf(out, "reac-master: [%.6f] %s -> %s (%s%s)\n", ts,
			        reac_master_state_name((enum reac_master_state)e.a),
			        reac_master_state_name((enum reac_master_state)e.b),
			        e.blk[0] == REAC_PEV_CAUSE_TIMER ? "timer"
			            : "rx ",
			        e.blk[0] == REAC_PEV_CAUSE_TIMER ? ""
			            : reac_master_rx_event_name((enum reac_master_rx_event)e.blk[0]));
			break;
		case REAC_PEV_JOIN:
			fmt_blk(hex, e.blk);
			fprintf(out, "reac-master: [%.6f] box JOIN seen (cdea 04 03, %s from %s)"
			        " -> %s\n  blk: %s\n", ts,
			        e.a ? "broadcast" : "unicast", mac,
			        reac_master_state_name((enum reac_master_state)e.b), hex);
			break;
		case REAC_PEV_RX:
			if ((enum reac_master_rx_event)e.a == REAC_M_RX_BOX_BYE) {
				fmt_blk(hex, e.blk);
				fprintf(out, "reac-master: [%.6f] box BYE (hb sel=0x00) from %s "
				        "(state %s)\n  blk: %s\n", ts, mac,
				        reac_master_state_name((enum reac_master_state)e.b), hex);
			} else {
				uint32_t lat_us = (uint32_t)e.blk[0] | ((uint32_t)e.blk[1] << 8) |
				                  ((uint32_t)e.blk[2] << 16) | ((uint32_t)e.blk[3] << 24);
				if (lat_us)
					fprintf(out, "reac-master: [%.6f] rx %s from %s (state %s, "
					        "%.1f ms after last chanmap)\n", ts,
					        reac_master_rx_event_name((enum reac_master_rx_event)e.a),
					        mac,
					        reac_master_state_name((enum reac_master_state)e.b),
					        (double)lat_us / 1000.0);
				else
					fprintf(out, "reac-master: [%.6f] rx %s from %s (state %s)\n",
					        ts,
					        reac_master_rx_event_name((enum reac_master_rx_event)e.a),
					        mac,
					        reac_master_state_name((enum reac_master_state)e.b));
			}
			break;
		case REAC_PEV_PRESENCE:
			if (e.a)
				fprintf(out, "reac-master: [%.6f] box presence GAINED "
				        "(%s from %s)\n", ts,
				        reac_master_rx_event_name((enum reac_master_rx_event)e.b),
				        mac);
			else
				fprintf(out, "reac-master: [%.6f] box presence LOST "
				        "(%d frames silent)\n", ts, REAC_M_PRESENCE_TIMEOUT);
			break;
		case REAC_PEV_GRANT_TIMEOUT:
			fprintf(out, "reac-master: [%.6f] GRANT window expired with no box "
			        "unicast — returning to PROBING (attempt #%u)\n", ts, e.a);
			break;
		case REAC_PEV_DROP:
			fprintf(out, "reac-master: [%.6f] ESTABLISHED -> %s (drop: %s)\n", ts,
			        reac_master_state_name((enum reac_master_state)e.b),
			        reac_master_drop_name((enum reac_master_drop_reason)e.a));
			break;
		case REAC_PEV_WATCHDOG: {
			uint64_t frames = atomic_load_explicit(&p->rx_box_frames,
			                                       memory_order_relaxed);
			uint64_t joins  = atomic_load_explicit(&p->rx_joins,
			                                       memory_order_relaxed);
			if (frames == 0)
				fprintf(out, "reac-master: [%.6f] still PROBING: rx_box_frames=0 "
				        "rx_joins=0 (wire silent — check the RX path)\n", ts);
			else
				fprintf(out, "reac-master: [%.6f] still PROBING: "
				        "rx_box_frames=%llu rx_joins=%llu (%s — bounce the box "
				        "PHY: it only cold-connects on link-up)\n", ts,
				        (unsigned long long)frames, (unsigned long long)joins,
				        e.a ? "box present, not joining" : "no sustained presence");
			break;
		}
		case REAC_PEV_RECOGNIZED: {
			/* b carries the matrix model index+1 (0 = no row names it) — never
			 * reac_box_model_by_channels here: its S-1608 fallback would NAME a
			 * box that only declared a width. */
			const struct reac_box_model *bm = reac_disco_model_by_index((int)e.b - 1);
			if (bm)
				fprintf(out, "reac-master: [%.6f] recognized box = %s from %s\n",
				        ts, bm->display, mac);
			else
				fprintf(out, "reac-master: [%.6f] box declared %u inputs from %s "
				        "(no matrix row — sized from the declaration)\n",
				        ts, (unsigned)e.a, mac);
			break;
		}
		case REAC_PEV_CLOCK: {
			/* The reference in use is NEVER implicit (#75). Every change of source
			 * or state prints role + pace + reference + the device behind it, and
			 * free-run is printed as free-run — we never dress it up as lock.
			 * The pacer is the master path by construction; the slave path runs no
			 * pacer, so no slave line can ever come out of here. */
			char line[224];
			int32_t applied = (int32_t)((uint32_t)e.blk[0] |
			                            ((uint32_t)e.blk[1] << 8) |
			                            ((uint32_t)e.blk[2] << 16) |
			                            ((uint32_t)e.blk[3] << 24));
			char label[REAC_CLOCK_LABEL_MAX];
			memcpy(label, e.blk + 4, REAC_CLOCK_LABEL_MAX);
			label[REAC_CLOCK_LABEL_MAX - 1] = '\0';
			/* State and quality were packed into one byte at the RT end (#77). */
			reac_clock_describe_full(REAC_ROLE_MASTER,
			                         (enum reac_clock_source)e.a,
			                         (enum reac_clock_state)(e.b & 0x0f),
			                         (enum reac_clock_quality)(e.b >> 4),
			                         label, line, sizeof line);
			/* Derived, not read across the thread boundary: the applied ppm in the
			 * event and the immutable nominal period give the steered period
			 * exactly (reac_dll_period_ns' formula). */
			double ppm = (double)applied / 1000.0;
			long steered = (long)((double)p->period_ns / (1.0 + ppm / 1e6) + 0.5);
			fprintf(out, "reac-clock: [%.6f] %s (applied %+.3f ppm, "
			        "period %ld ns vs nominal %ld ns)\n", ts, line,
			        ppm, steered, p->period_ns);
			break;
		}
		case REAC_PEV_SIGHTING: {
			/* MAIN THREAD: fold the sighting into the discovery table the sink node
			 * publishes from. `owned` is decided HERE, against the master's current
			 * peer, so the pacer thread never has to reason about ownership. */
			struct reac_disco_sighting s;
			memset(&s, 0, sizeof s);
			memcpy(s.mac, e.src, 6);
			s.role = (enum reac_disco_role)e.a;
			s.model = reac_disco_model_by_index((int)e.b - 1);
			int owned = (p->master.state == REAC_M_ESTABLISHED &&
			             memcmp(p->master.box_mac, e.src, 6) == 0);
			/* A box declares its model ONCE, at enrolment. Every frame after that is
			 * audio FILLER carrying no identity, so passive classification of a
			 * long-established box yields role=box / model=unknown — verified live on
			 * the rig: 24001 frames in 3 s, not one config-announce among them. For
			 * OUR peer the model is not unknown at all: the master matched its
			 * config-announce byte-for-byte at enrolment and publishes it as
			 * reac.box-model. Reuse that rather than let discovery report "unknown"
			 * for the very box the stagebox badge beside it names. Still no
			 * inference — recognized_box is only ever a byte-exact match. */
			if (!s.model && owned)
				s.model = atomic_load_explicit(&p->recognized_box,
				                               memory_order_acquire);
			if (reac_disco_table_observe(&p->disco, &s, owned, e.mono_ns))
				fprintf(out, "reac-disco: [%.6f] %s %s model=%s%s\n", ts,
				        reac_disco_role_name(s.role), mac,
				        s.model ? s.model->token : "unknown",
				        owned ? " (ours)" : "");
			break;
		}
		default:
			fprintf(out, "reac-master: [%.6f] event kind %u a=%u b=%u\n",
			        ts, e.kind, e.a, e.b);
			break;
		}
	}

	/* Fix 2a: ring-depth telemetry, GATED so the long-lived master's journald log
	 * isn't flooded. The depth SAWTOOTHS with each producer burst (measured live
	 * ~38..113 frames), so a per-change band is useless — it fires every drain.
	 * Emit the depth line ONLY on the ~10 s heartbeat (a periodic health marker
	 * carrying the interval min/max/last depth) or when the guard trimmed this
	 * interval (rare + important). min/max are read-and-reset only when we emit,
	 * so each line covers the whole interval, not just the last 200 ms. NOT counted
	 * as a drained event (callers key off the event count). */
	uint64_t rtrims = atomic_load_explicit(&p->ring_trims, memory_order_relaxed);
	uint64_t now = mono_ns();

	/* Withdraw devices that have gone silent. Aging runs on the DRAIN cadence, not on
	 * arrival, so a box that stops talking altogether — the case where no frame ever
	 * arrives to trigger anything — still disappears. A stagebox vanishing with the UI
	 * still showing it is precisely the silent-failure this exists to prevent. */
	int gone = reac_disco_table_age(&p->disco, now);
	if (gone)
		fprintf(out, "reac-disco: [%.6f] %d device(s) went silent > %llu s — withdrawn\n",
		        (double)now / 1e9, gone,
		        (unsigned long long)(REAC_DISCO_STALE_NS / 1000000000ULL));
	/* A SUSTAINED trim is not bounded-latency housekeeping, it is a RATE
	 * MISMATCH, and it must say so instead of hiding as a rising number in the
	 * telemetry line below. The guard exists because the graph clock can run
	 * marginally fast against the wire, which trims RARELY. When the wire and the
	 * graph are at different RATES the producer outruns the consumer forever, so
	 * the guard trims on drain after drain and throws away ~100 ms of audio each
	 * time — the "extremely saturated" sound, with nothing in the log that names
	 * a cause. reac-pw has no TX resampler, so this is the only honest warning we
	 * can give. Once per run: an operator who reads it can act, and a repeat every
	 * drain would be the same flood the gate above exists to prevent. */
	if (rtrims != p->log_last_trims) {
		if (++p->trim_run >= 20 && !p->trim_warned) {
			uint64_t rf = atomic_load_explicit(&p->ring_trim_frames,
			                                   memory_order_relaxed);
			p->trim_warned = 1;
			fprintf(out, "reac-pacer: WIRE/GRAPH RATE MISMATCH — the guard has "
			        "trimmed on %u consecutive drains (%llu trims, %llu frames "
			        "dropped). This is not latency housekeeping: the graph is "
			        "producing faster than the wire consumes, so audio is being "
			        "discarded in ~100 ms chunks. reac-pw has NO TX resampler — "
			        "run the wire at the graph's rate (--rate) or the audio stays "
			        "chopped.\n", p->trim_run,
			        (unsigned long long)rtrims, (unsigned long long)rf);
		}
	} else {
		p->trim_run = 0;
	}

	int emit = (rtrims != p->log_last_trims) ||
	           (now - p->log_last_ns >= REAC_PACER_DEPTH_LOG_HB_NS);
	if (emit) {
		uint32_t rdepth = reac_frame_ring_readable(&p->ring);
		uint32_t rmax = atomic_exchange_explicit(&p->ring_depth_peak, rdepth,
		                                         memory_order_relaxed);
		uint32_t rmin = atomic_exchange_explicit(&p->ring_depth_min, rdepth,
		                                         memory_order_relaxed);
		if (rmin > rmax)   /* nothing recorded yet (pre-first-slot): flatten to now */
			rmin = rmax = rdepth;
		uint64_t rframes = atomic_load_explicit(&p->ring_trim_frames,
		                                        memory_order_relaxed);
		double slot_ms = (double)p->period_ns / 1e6;
		fprintf(out, "reac-pacer: ring depth %u frames (%.2f ms) [interval min %u "
		        "(%.2f ms) max %u (%.2f ms)] | guard trims=%llu dropped=%llu frames\n",
		        rdepth, (double)rdepth * slot_ms,
		        rmin, (double)rmin * slot_ms, rmax, (double)rmax * slot_ms,
		        (unsigned long long)rtrims, (unsigned long long)rframes);
		p->log_last_ns = now;
		p->log_last_trims = rtrims;
	}

	return count;
}

/* ---- live head-amp control handoff (task #203) --------------------------- *
 * A controller (openmixer) sets a per-channel phantom/pad/sens prop on the
 * master node; the change must reach the box preamps at runtime. The head-amp
 * send TABLE (reac_headamp_tx) is read AND advanced by the RT pacer thread every
 * emit (reac_headamp_tx_next clears dirty flags + walks the re-assert sweep), so
 * it must stay single-writer on that thread — a second writer from the loop
 * thread would race the sweep cursor and the dirty bitmap. Instead the loop
 * thread PACKS its change into one 32-bit word and pushes it through this SPSC
 * ring; the RT thread drains + applies it via reac_headamp_tx_set (still the sole
 * writer of the table). One atomic word per command means the RT reader can never
 * observe a torn (ch,param,value). Same lock-free idiom as the event ring, roles
 * reversed (loop = producer, pacer = consumer). */

int reac_pacer_headamp_set(struct reac_pacer *p, uint8_t ch, uint8_t param,
                           uint8_t value)
{
	uint32_t h = atomic_load_explicit(&p->ha_cmd_head, memory_order_relaxed);
	uint32_t t = atomic_load_explicit(&p->ha_cmd_tail, memory_order_acquire);
	if (h - t >= REAC_HEADAMP_CMD_RING) {
		/* Full: drop this command. The DMX re-assert model makes this benign —
		 * an absolute value is not a delta, so a later set of the same cell
		 * supersedes it wholesale; nothing accumulates a wrong offset. */
		atomic_fetch_add_explicit(&p->ha_cmd_drops, 1, memory_order_relaxed);
		return 0;
	}
	atomic_store_explicit(&p->ha_cmd[h % REAC_HEADAMP_CMD_RING],
	                      reac_headamp_pack(ch, param, value),
	                      memory_order_relaxed);
	/* Release: publish the cell store before the consumer can observe the new
	 * head index (pairs with the acquire load in the drain below). */
	atomic_store_explicit(&p->ha_cmd_head, h + 1, memory_order_release);
	return 1;
}

int reac_pacer_headamp_drain(struct reac_pacer *p)
{
	int applied = 0;
	uint32_t t = atomic_load_explicit(&p->ha_cmd_tail, memory_order_relaxed);
	for (;;) {
		uint32_t h = atomic_load_explicit(&p->ha_cmd_head, memory_order_acquire);
		if (t == h)
			break;
		uint32_t w = atomic_load_explicit(&p->ha_cmd[t % REAC_HEADAMP_CMD_RING],
		                                  memory_order_relaxed);
		atomic_store_explicit(&p->ha_cmd_tail, ++t, memory_order_release);
		uint8_t ch, param, value;
		reac_headamp_unpack(w, &ch, &param, &value);
		/* reac_headamp_tx_set validates (ch/param/value) and silently rejects a
		 * bad triple, arms the table, and marks the cell dirty so the change goes
		 * out as an edge on the next eligible FILLER slot, then re-asserts. Pure
		 * array writes — no alloc, no syscall — RT-safe. */
		reac_headamp_tx_set(&p->headamp, ch, param, value);
		applied++;
	}
	if (applied)
		atomic_fetch_add_explicit(&p->ha_cmd_applied, (uint64_t)applied,
		                          memory_order_relaxed);
	return applied;
}

/* ---- clock discipline (#75) --------------------------------------------- *
 * The pacer has always free-run on CLOCK_MONOTONIC. That is correct only when
 * nothing else on the rig is the clock master; the moment a word-clock-locked
 * converter, a PHC, or a stagebox fed from a house clock is the reference, our
 * cadence and the true clock walk apart — small per second, inexorable over a
 * set, and heard as periodic resampling artefacts or a link that behaves for
 * twenty minutes and then does not.
 *
 * This closes the loop we were already measuring: reac_rx is the rate authority
 * and already publishes a filtered ppm error (the source node feeds it to
 * PipeWire's io_rate_match). We simply never steered TX with it. */

int reac_clock_label_set(struct reac_clock_label *l, const char *name)
{
	if (!name)
		return 0;
	if (strncmp(l->name, name, REAC_CLOCK_LABEL_MAX) == 0)
		return 0;                     /* the common case: nothing to publish */
	uint32_t s = atomic_load_explicit(&l->seq, memory_order_relaxed);
	atomic_store_explicit(&l->seq, s + 1, memory_order_release);   /* odd: writing */
	strncpy(l->name, name, REAC_CLOCK_LABEL_MAX - 1);
	l->name[REAC_CLOCK_LABEL_MAX - 1] = '\0';
	atomic_store_explicit(&l->seq, s + 2, memory_order_release);   /* even: done */
	return 1;
}

int reac_clock_label_get(const struct reac_clock_label *l, char *out, size_t cap)
{
	uint32_t before = atomic_load_explicit(&l->seq, memory_order_acquire);
	if (before & 1u)
		return 0;                     /* a write is in flight */
	strncpy(out, l->name, cap - 1);
	out[cap - 1] = '\0';
	uint32_t after = atomic_load_explicit(&l->seq, memory_order_acquire);
	return before == after;           /* 0 = torn; print nothing, not half a name */
}

void reac_pacer_clock_publish(struct reac_pacer *p, enum reac_clock_source src,
                              int present, int ppm_milli, const char *label,
                              enum reac_clock_quality quality, uint64_t now_ns)
{
	if ((unsigned)src >= REAC_CLOCK_SRC_COUNT || src == REAC_CLOCK_SRC_FREERUN)
		return;   /* free-run is not published; it is what "nothing" means */
	if (present) {
		reac_clock_label_set(&p->clock_label[src], label);
		atomic_store_explicit(&p->clock_quality[src], (int)quality,
		                      memory_order_relaxed);
		atomic_store_explicit(&p->clock_ppm_milli[src], ppm_milli,
		                      memory_order_relaxed);
		/* Release: the stamp is what the consumer keys freshness off, so it must
		 * become visible AFTER the value it vouches for. */
		atomic_store_explicit(&p->clock_stamp_ns[src], now_ns, memory_order_release);
		atomic_fetch_or_explicit(&p->clock_present, 1u << src, memory_order_relaxed);
	} else {
		atomic_fetch_and_explicit(&p->clock_present, ~(1u << src),
		                          memory_order_relaxed);
	}
}

long reac_pacer_clock_tick(struct reac_pacer *p, uint64_t now_ns)
{
	/* INERT. Not "the discipline decided to do nothing" — the discipline is never
	 * consulted, so the deadline advances by exactly the constant it always did. */
	if (!p->clock_follow)
		return p->period_ns;

	if (++p->clock_slots < REAC_CLOCK_TICK_SLOTS)
		return p->slot_period_ns;
	p->clock_slots = 0;

	/* What is actually available right now: a presence bit AND a sample no older
	 * than REAC_CLOCK_STALE_NS. A publisher that died leaves its bit set; ageing
	 * it out here is what turns that into an honest holdover instead of a lock
	 * claim against a reference that stopped talking. */
	uint32_t present = atomic_load_explicit(&p->clock_present, memory_order_relaxed);
	uint32_t avail = 0;
	for (int s = 1; s < REAC_CLOCK_SRC_COUNT; s++) {
		if (!(present & (1u << s)))
			continue;
		uint64_t stamp = atomic_load_explicit(&p->clock_stamp_ns[s],
		                                      memory_order_acquire);
		if (stamp && now_ns - stamp < REAC_CLOCK_STALE_NS)
			avail |= 1u << s;
		/* Hand the publisher's inferred grade to the discipline (#77) before it
		 * selects, so a structurally disqualified reference is skipped in the same
		 * walk that skips an absent one. */
		reac_clock_disc_set_quality(&p->clock, (enum reac_clock_source)s,
		                            (enum reac_clock_quality)
		                            atomic_load_explicit(&p->clock_quality[s],
		                                                 memory_order_relaxed));
	}

	/* Only a NEW sample steers the loop: re-feeding the same estimate every tick
	 * would inflate the update count and let a stalled publisher's last value look
	 * like continuous evidence of lock. */
	/* The SAME selection the discipline is about to make — graded, so the source
	 * we test for a fresh sample can never be one the discipline then refuses. */
	enum reac_clock_source sel = reac_clock_select_graded(p->clock.role, avail,
	                                                      p->clock.quality,
	                                                      p->clock.min_quality);
	double ppm = 0.0;
	int have = 0;
	if (sel != REAC_CLOCK_SRC_FREERUN) {
		uint64_t stamp = atomic_load_explicit(&p->clock_stamp_ns[sel],
		                                      memory_order_acquire);
		if (stamp != p->clock_stamp_seen[sel]) {
			p->clock_stamp_seen[sel] = stamp;
			ppm = (double)atomic_load_explicit(&p->clock_ppm_milli[sel],
			                                   memory_order_relaxed) / 1000.0;
			have = 1;
		}
	}

	reac_clock_disc_update(&p->clock, avail, ppm, have);

	/* The reference in use must never be implicit: every change of source or state
	 * goes into the transcript. RT-safe — the event ring, same as every other
	 * pacer-thread log. */
	if (p->clock.generation != p->clock_gen_seen) {
		p->clock_gen_seen = p->clock.generation;
		int32_t applied = (int32_t)(reac_dll_applied_ppm(&p->clock.dll) * 1000.0);
		uint8_t blk[32] = { 0 };
		blk[0] = (uint8_t)applied;         blk[1] = (uint8_t)(applied >> 8);
		blk[2] = (uint8_t)(applied >> 16); blk[3] = (uint8_t)(applied >> 24);
		/* Name the actual device in the same event, so the drain never has to
		 * reach back into pacer-thread state to format the line. */
		if (p->clock.src != REAC_CLOCK_SRC_FREERUN)
			reac_clock_label_get(&p->clock_label[p->clock.src],
			                     (char *)blk + 4, REAC_CLOCK_LABEL_MAX);
		/* State and quality share `b`: blk is full (4 bytes of ppm + the 28-byte
		 * label) and both enums are well under 16 values. Unpacked in the drain. */
		uint8_t b = (uint8_t)(p->clock.state |
		                      (reac_clock_disc_quality(&p->clock) << 4));
		pev_push(p, REAC_PEV_CLOCK, (uint8_t)p->clock.src, b, NULL, blk);
	}

	p->slot_period_ns = reac_clock_disc_period_ns(&p->clock);
	return p->slot_period_ns;
}

/* ---- the RT pacer thread ------------------------------------------------ */

static void *pacer_loop(void *arg)
{
	struct reac_pacer *p = arg;

	/* RT setup (the reac_repacer.c recipe): lock memory, pin, go SCHED_FIFO.
	 * Best-effort — if we lack privilege the thread still runs at SCHED_OTHER
	 * (jittery but functional for the loopback demo). */
	mlockall(MCL_CURRENT | MCL_FUTURE);
	if (p->cpu >= 0) {
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(p->cpu, &set);
		sched_setaffinity(0, sizeof set, &set);
	}
	struct sched_param sp = { .sched_priority = p->prio };
	if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
		fprintf(stderr, "reac_pacer: SCHED_FIFO denied (need CAP_SYS_NICE / rtprio); "
		                "running SCHED_OTHER — cadence may jitter\n");

	/* Block all signals on this thread. SIGINT/SIGTERM are serviced by the pw/main
	 * loop, not here; a signal delivered to this thread would only cut the slot
	 * sleep short (EINTR) and emit a frame ahead of cadence. */
	sigset_t allsig;
	sigfillset(&allsig);
	pthread_sigmask(SIG_BLOCK, &allsig, NULL);

	uint8_t frame[REAC_FRAME_BYTES];
	uint8_t popbuf[2048];
	uint8_t rxbuf[2048];

	uint64_t deadline = mono_ns() + (uint64_t)p->period_ns;
	atomic_store_explicit(&p->started, 1, memory_order_release);

	struct sockaddr_ll sll;
	memset(&sll, 0, sizeof sll);
	sll.sll_family  = AF_PACKET;
	sll.sll_ifindex = p->ifindex;
	sll.sll_halen   = 6;
	memset(sll.sll_addr, 0xFF, 6);  /* broadcast dst */

	while (atomic_load_explicit(&p->running, memory_order_acquire)) {
		struct timespec d = { deadline / 1000000000ull, deadline % 1000000000ull };
		/* Re-arm the SAME absolute deadline if interrupted (belt-and-braces: we
		 * also block all signals above). An EINTR return means the slot sleep was
		 * cut short — sleeping again to the same absolute target keeps cadence;
		 * just emitting would put a frame ahead of the beat. */
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &d, NULL) == EINTR)
			;

		/* Bounded non-blocking RX drain: apply the box's frames to the FSM
		 * BEFORE deciding this slot's emission. Budget 8 = 8x wire-rate
		 * headroom per slot (the box floods <=1 frame/slot on average); a
		 * kernel-queue overflow only costs box FILLER, and a dropped JOIN is
		 * retried by the box on its ~100 ms grid. recv+classify is a handful
		 * of byte loads — same syscall class as the sendto below; no
		 * allocation, no locks, no stdio (events go to the lock-free ring). */
		for (int i = 0; i < REAC_PACER_RX_BUDGET; i++) {
			ssize_t rn = recv(p->fd, rxbuf, sizeof rxbuf, MSG_DONTWAIT);
			if (rn <= 0)
				break;                      /* EAGAIN = drained */
			reac_pacer_rx_ingest(p, rxbuf, (size_t)rn);
		}

		/* Bound the graph->wire buffering (task #152). The graph clock can run
		 * marginally fast vs. the fixed wire clock, walking the ring depth up over
		 * long uptime toward the ~250 ms cap. As the ring's consumer (we own tail)
		 * drop the OLDEST excess back to TARGET once depth exceeds HIGH — latency
		 * stays bounded instead of drifting. HIGH is derived from the ACTUAL
		 * producer burst (the graph pushes up to one quantum = quantum/12 frames per
		 * callback, so the depth sawtooths by that much): HIGH clears several bursts
		 * so normal operation NEVER trims; TARGET = HIGH/2. Also track the depth
		 * min/max for the non-RT telemetry drain. All a handful of atomic ops;
		 * RT-safe. */
		uint32_t depth = reac_frame_ring_readable(&p->ring);
		if (depth > atomic_load_explicit(&p->ring_depth_peak, memory_order_relaxed))
			atomic_store_explicit(&p->ring_depth_peak, depth, memory_order_relaxed);
		if (depth < atomic_load_explicit(&p->ring_depth_min, memory_order_relaxed))
			atomic_store_explicit(&p->ring_depth_min, depth, memory_order_relaxed);
		uint32_t qframes = atomic_load_explicit(&p->graph_quantum,
		                                        memory_order_relaxed)
		                   / (uint32_t)REAC_SAMPLES_PER_PKT;
		uint32_t high = reac_pacer_guard_high(qframes);
		uint32_t trimmed = reac_frame_ring_trim(&p->ring, high, high / 2);
		if (trimmed) {
			atomic_fetch_add_explicit(&p->ring_trims, 1, memory_order_relaxed);
			atomic_fetch_add_explicit(&p->ring_trim_frames, trimmed,
			                          memory_order_relaxed);
		}

		/* Pull the next encoded frame; on underrun emit a silent FILLER so the
		 * slot — and the master sequence — never stalls. */
		uint16_t n = reac_frame_ring_pop(&p->ring, popbuf);
		if (n >= REAC_FRAME_BYTES) {
			memcpy(frame, popbuf, REAC_FRAME_BYTES);
		} else {
			build_silent_filler(frame, p->src);
		}

		/* Advance the master FSM by one frame; stamp the counter + control block.
		 * This turns a plain audio FILLER into the right cdea/cfea control frame
		 * (probe / grant / chanmap / announce) when the sequence calls for it. */
		uint16_t counter;
		int tmpl_idx;
		enum reac_master_emit emit = reac_master_next(&p->master, &counter, &tmpl_idx);
		frame[REAC_HDR_COUNTER_OFF]     = (uint8_t)(counter & 0xFF);
		frame[REAC_HDR_COUNTER_OFF + 1] = (uint8_t)((counter >> 8) & 0xFF);
		reac_master_stamp(&p->master, frame, emit, tmpl_idx);
		if (emit == REAC_M_EMIT_CHANMAP)
			p->last_chanmap_ns = mono_ns();     /* hb-after-walk latency base */

		/* Absorb any LIVE head-amp control changes a controller pushed since the
		 * last slot (task #203). Drained on THIS thread so the head-amp table stays
		 * single-writer here: reac_pacer_headamp_set only enqueues; we apply. Run
		 * every slot regardless of FSM state so a change is already in the table the
		 * instant we reach an ESTABLISHED FILLER slot below — cheap when the ring is
		 * empty (two atomic loads). */
		reac_pacer_headamp_drain(p);

		/* MASTER head-amp overlay (task #155), STRICTLY GUARDED so it can never
		 * touch establishment: it only ever OVERWRITES a FILLER slot, and only once
		 * ESTABLISHED. It never replaces a PROBE/GRANT/CHANMAP/CFEA/ENROLL frame, so
		 * the verified grant/chanmap/announce cadence (reac_master_next) is
		 * untouched. It fires for a pending operator edge OR for the one-shot
		 * complete-scene replay note_transition arms at every establishment (the
		 * power-cycle restore); once both are drained the wire goes silent again.
		 * The counter + audio the frame already carries are preserved (stamp only
		 * rewrites the control block [16:50]). */
		if (emit == REAC_M_EMIT_FILLER &&
		    p->master.state == REAC_M_ESTABLISHED &&
		    (p->headamp.active || p->headamp.replay_width)) {
			uint8_t hch, hparam, hval;
			if (reac_headamp_tx_next(&p->headamp, &hch, &hparam, &hval))
				reac_ctrl_stamp_headamp(frame, hch, hparam, hval);
		}

		/* Timer-driven backward transitions (the undeclared-box hold, peer-gone
		 * budget) happen inside reac_master_next — mirror + log them here. */
		if (p->master.state != p->prev_state) {
			note_transition(p, p->prev_state, p->master.state,
			                REAC_PEV_CAUSE_TIMER);
			sync_published_box(p);   /* a peer-gone drop forgets the box */
		}
		/* presence LOST edge (the 600-frame decay ran out in next()) */
		if (!p->master.box_seen && p->presence_seen) {
			pev_push(p, REAC_PEV_PRESENCE, 0, 0, NULL, NULL);
			p->presence_seen = 0;
		}
		/* fruitless-probing watchdog: one summary line every 10 s */
		if (p->master.state == REAC_M_PROBING) {
			if (++p->probing_slots % ((uint64_t)p->fps * 10) == 0)
				pev_push(p, REAC_PEV_WATCHDOG,
				         (uint8_t)p->master.box_seen, 0, NULL, NULL);
		} else {
			p->probing_slots = 0;
		}

		/* Non-blocking send (the socket carries SOCK_NONBLOCK). The pacer runs
		 * SCHED_FIFO: a blocking sendto() on a backed-up NIC tx queue would stall
		 * THIS thread mid-period and smear the cadence the pacer exists to protect.
		 * On EAGAIN/EWOULDBLOCK we drop this slot (bump tx_errors) and move on — the
		 * absolute-deadline snap-forward below keeps the next slot on time. */
		ssize_t r = sendto(p->fd, frame, REAC_FRAME_BYTES, MSG_DONTWAIT,
		                   (struct sockaddr *)&sll, sizeof sll);
		if (r < 0)
			atomic_fetch_add_explicit(&p->tx_errors, 1, memory_order_relaxed);
		else
			atomic_fetch_add_explicit(&p->tx_frames, 1, memory_order_relaxed);

		/* Advance the absolute deadline by exactly one period (no drift). If we
		 * woke a full period or more late (scheduler hiccup), snap forward so we
		 * don't burst-catch-up and smear the cadence.
		 *
		 * The period is a CONSTANT unless clock following is enabled (#75). With
		 * the knob unset this is `p->period_ns`, the same expression as before —
		 * reac_pacer_clock_tick is not even entered, so the cadence and every
		 * emitted byte are identical to a build without this code. When enabled,
		 * the discipline steers the period CONTINUOUSLY and the deadline still
		 * advances by exactly one period: the phase is never stepped, so a
		 * correction is a slow pull rather than an audible click. */
		deadline += (uint64_t)(p->clock_follow ? reac_pacer_clock_tick(p, mono_ns())
		                                       : p->period_ns);
		uint64_t now = mono_ns();
		if (now > deadline) {
			atomic_fetch_add_explicit(&p->late_wakes, 1, memory_order_relaxed);
			deadline = now + (uint64_t)p->period_ns;
		}
	}
	return NULL;
}

/* ---- lifecycle ---------------------------------------------------------- */

int reac_pacer_open(struct reac_pacer *p, const struct reac_pacer_cfg *cfg)
{
	memset(p, 0, sizeof *p);
	p->fd = -1;
	p->prio = cfg->prio > 0 ? cfg->prio : 79;
	p->cpu  = cfg->cpu;
	p->fps  = cfg->fps > 0 ? cfg->fps : 8000;
	p->period_ns = reac_pacer_period_ns(cfg->fps);
	p->prev_state = REAC_M_IDLE;
	atomic_store_explicit(&p->fsm_state, REAC_M_IDLE, memory_order_relaxed);

	/* Clock discipline (#75). The object is always initialised — it costs a memset
	 * — but it is only ever CONSULTED when the knob is set. With clock_follow == 0
	 * the pacer advances its deadline by p->period_ns exactly as it always has.
	 * The pacer is the MASTER path by definition (a slave runs no pacer; frame
	 * arrival is its slot clock), hence the master hierarchy. */
	p->clock_follow = cfg->clock_follow;
	p->slot_period_ns = p->period_ns;
	reac_clock_disc_init(&p->clock, REAC_ROLE_MASTER, p->period_ns);
	if (p->clock_follow) {
		char line[160];
		fprintf(stderr, "reac-clock: following ENABLED (REACPW_CLOCK_FOLLOW) — %s, "
		        "nominal period %ld ns; the reference in use is reported on every "
		        "change\n",
		        reac_clock_disc_describe(&p->clock, line, sizeof line),
		        p->period_ns);
	}
	/* min starts at "unset" so the pacer thread's first slot records the true low;
	 * the drain flattens min>max to the current depth if no slot has run yet. */
	atomic_store_explicit(&p->ring_depth_min, UINT32_MAX, memory_order_relaxed);

	/* Default source MAC: the Roland OUI + this NIC's host part (reac_mac.h). The
	 * master path in main.c always supplies cfg->src_mac (the impersonated desk's
	 * MAC), so this is a defensive fallback that keeps the collision-safe default
	 * in one helper rather than a scattered hard-coded host part. */
	if (cfg->src_mac)
		memcpy(p->src, cfg->src_mac, 6);
	else
		reac_mac_default_src(cfg->ifname, p->src);

	/* A zero out_channels means the caller left the console cfg unset -> the
	 * S-1608 default (reac_master_init(NULL)). */
	const struct reac_console_cfg *ccfg =
		cfg->console.out_channels ? &cfg->console : NULL;
	reac_master_init(&p->master, p->src, ccfg, cfg->fps);

	/* MASTER head-amp DMX send table (task #155). Off unless the caller passes at
	 * least one setting: an all-unset table's next() always returns 0, so the
	 * downstream stays byte-identical to a no-head-amp master. Loaded here (before
	 * the pacer thread starts) so the table is single-writer from the RT thread on
	 * — no cross-thread mutation, no race with establishment. */
	reac_headamp_tx_init(&p->headamp);
	for (int i = 0; i < cfg->n_headamps; i++)
		reac_headamp_tx_set(&p->headamp, cfg->headamps[i].ch,
		                    cfg->headamps[i].param, cfg->headamps[i].value);

	/* Point the master's GRANT sweep at this table: group A of the enrollment sweep
	 * IS the initial head-amp state push (reac_grant.h), so the state we enroll a
	 * box with must be the state the operator configured — not a second, divergent
	 * copy. Borrowed pointer; both live in `p` and are touched only by the pacer
	 * thread once running, so the sweep and the DMX re-assert always agree.
	 * Rebuilt again on box recognition (reac_master_set_box) and by any later live
	 * change, since the sweep is only consumed at grant time. */
	reac_master_set_headamp_src(&p->master, &p->headamp);

	/* ~250 ms of frame ring at this rate (power-of-two rounded inside init). */
	uint32_t depth = (uint32_t)(cfg->fps / 4);
	if (depth < 8)
		depth = 8;
	if (reac_frame_ring_init(&p->ring, depth, 2048) != 0)
		return -1;

	/* SOCK_NONBLOCK so the RT pacer thread's sendto() can never block on a backed-up
	 * NIC tx queue (it also passes MSG_DONTWAIT per-send; either alone suffices). */
	int fd = socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK, htons(REAC_ETHERTYPE));
	if (fd < 0) {
		reac_frame_ring_free(&p->ring);
		return -1;
	}
	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, cfg->ifname, IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
		close(fd);
		reac_frame_ring_free(&p->ring);
		return -1;
	}
	p->ifindex = ifr.ifr_ifindex;

	/* Bind to the interface (the reac_slave_open pattern): this fd also RXes
	 * the box's upstream control frames for the per-slot drain, and unbound it
	 * would deliver 0x8819 from EVERY NIC. */
	struct sockaddr_ll bsll;
	memset(&bsll, 0, sizeof bsll);
	bsll.sll_family = AF_PACKET;
	bsll.sll_protocol = htons(REAC_ETHERTYPE);
	bsll.sll_ifindex = p->ifindex;
	if (bind(fd, (struct sockaddr *)&bsll, sizeof bsll) < 0) {
		close(fd);
		reac_frame_ring_free(&p->ring);
		return -1;
	}

	/* PROMISCUOUS mode is MANDATORY for the master role. We TX with a SPOOFED
	 * Roland-OUI src MAC (our mixer identity), NOT the NIC's hardware MAC. A box
	 * ESTABLISHES by UNICASTING its config-announce + upstream to THAT spoofed
	 * MAC — which the NIC's hardware filter drops (it isn't the card's real MAC),
	 * so without promisc the master receives nothing and stays PROBING forever
	 * while the box streams to us (verified live: real S-0808 -> reac-pw master,
	 * 31.9k unicast frames on the mirror, rx_box_frames=1 without promisc). */
	{
		struct packet_mreq mr;
		memset(&mr, 0, sizeof mr);
		mr.mr_ifindex = p->ifindex;
		mr.mr_type    = PACKET_MR_PROMISC;
		if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof mr) < 0)
			fprintf(stderr, "reac_pacer: PACKET_MR_PROMISC failed — the master "
			                "may not see a box's unicast to our spoofed MAC\n");
	}

	/* Best-effort (Linux >=4.20): don't echo our own 8000 fps broadcast into
	 * our RX queue. The classifier's src==ours software filter stays mandatory
	 * either way (hub/loopback echoes, older kernels). */
#ifdef PACKET_IGNORE_OUTGOING
	{
		int one = 1;
		setsockopt(fd, SOL_PACKET, PACKET_IGNORE_OUTGOING, &one, sizeof one);
	}
#endif

	p->fd = fd;
	return 0;
}

int reac_pacer_start(struct reac_pacer *p)
{
	atomic_store_explicit(&p->running, 1, memory_order_release);
	if (pthread_create(&p->thread, NULL, pacer_loop, p) != 0) {
		atomic_store_explicit(&p->running, 0, memory_order_release);
		return -1;
	}
	return 0;
}

int reac_pacer_submit(struct reac_pacer *p, const uint8_t *frame, uint16_t n)
{
	return reac_frame_ring_push(&p->ring, frame, n);
}

void reac_pacer_stop(struct reac_pacer *p)
{
	if (!atomic_load_explicit(&p->running, memory_order_acquire))
		return;
	atomic_store_explicit(&p->running, 0, memory_order_release);
	pthread_join(p->thread, NULL);
}

void reac_pacer_close(struct reac_pacer *p)
{
	if (p->fd >= 0)
		close(p->fd);
	p->fd = -1;
	reac_frame_ring_free(&p->ring);
}
