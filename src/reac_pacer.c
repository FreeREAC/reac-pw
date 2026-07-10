// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* clock_nanosleep / TIMER_ABSTIME, sched_setscheduler */
#endif
#include "reac_pacer.h"
#include "reac_ctrl.h"     /* reac_ctrl_classify_box_frame */

#include <reac/reac.h>     /* REAC_FRAME_BYTES, REAC_HDR_COUNTER_OFF, ... */

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

static uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

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
	atomic_store_explicit(&p->fsm_state, to, memory_order_release);
	atomic_store_explicit(&p->grant_attempts, p->master.grant_attempts,
	                      memory_order_relaxed);
	memset(p->rx_since_change, 0, sizeof p->rx_since_change);
	p->probing_slots = 0;

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

void reac_pacer_rx_ingest(struct reac_pacer *p, const uint8_t *frame, size_t len)
{
	struct reac_ctrl_parsed parsed;
	enum reac_master_rx_event ev;
	if (reac_ctrl_classify_box_frame(frame, len, p->src, &parsed, &ev) != 0)
		return;

	atomic_fetch_add_explicit(&p->rx_box_frames, 1, memory_order_relaxed);
	if (parsed.kind != REAC_CTRL_FILLER)
		atomic_fetch_add_explicit(&p->rx_box_ctrl, 1, memory_order_relaxed);
	if (ev == REAC_M_RX_BOX_JOIN)
		atomic_fetch_add_explicit(&p->rx_joins, 1, memory_order_relaxed);

	enum reac_master_state from = p->master.state;
	int changed = reac_master_rx(&p->master, ev, parsed.src,
	                             ev == REAC_M_RX_BOX_JOIN ? frame + 18 : NULL);

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
		 * full; unicast heartbeats thereafter 1-in-8; FILLER presence only on
		 * edges (above). For an established heartbeat, stash the latency
		 * since our last chanmap walk in blk[0..3] LE (healthy lockstep is
		 * 0.5-1.9 ms in the golden capture). */
		uint32_t seen = p->rx_since_change[ev & 3]++;
		int log_it = (seen == 0) ||
		             (ev == REAC_M_RX_BOX_UNICAST && (seen & 7) == 0);
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
				fprintf(out, "reac-master: [%.6f] rx %s from %s (state %s"
				        "%s%.1f ms after last chanmap%s)\n", ts,
				        reac_master_rx_event_name((enum reac_master_rx_event)e.a),
				        mac,
				        reac_master_state_name((enum reac_master_state)e.b),
				        lat_us ? ", " : "", lat_us ? (double)lat_us / 1000.0 : 0.0,
				        lat_us ? "" : "");
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
		default:
			fprintf(out, "reac-master: [%.6f] event kind %u a=%u b=%u\n",
			        ts, e.kind, e.a, e.b);
			break;
		}
	}
	return count;
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

		/* Timer-driven backward transitions (grant-window expiry, peer-gone
		 * budget) happen inside reac_master_next — mirror + log them here. */
		if (p->master.state != p->prev_state)
			note_transition(p, p->prev_state, p->master.state,
			                REAC_PEV_CAUSE_TIMER);
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
		 * don't burst-catch-up and smear the cadence. */
		deadline += (uint64_t)p->period_ns;
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

	static const uint8_t standin[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };
	memcpy(p->src, cfg->src_mac ? cfg->src_mac : standin, 6);

	reac_master_init(&p->master, p->src, cfg->fps);

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
