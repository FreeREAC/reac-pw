// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* clock_nanosleep / TIMER_ABSTIME, sched_setscheduler */
#endif
#include "reac_pacer.h"

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
	p->period_ns = reac_pacer_period_ns(cfg->fps);

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
