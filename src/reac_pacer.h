// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_pacer — the SCHED_FIFO cadence pacer for the master-role TX path.
 *
 * A real REAC slave (stagebox) recovers its word clock from the master's frame
 * inter-arrival interval, so the downstream broadcast MUST leave at a rock-steady
 * pps (8000 @96k / 4000 @48k / 3675 @44.1k) or the box hears rate jitter and
 * eventually drops the link. The PipeWire graph thread cannot guarantee that: its
 * quantum is bursty and a sendto() syscall on the RT graph thread adds wake
 * jitter. So we move emission onto a dedicated thread, exactly the reac_repacer.c
 * pattern: mlockall, SCHED_FIFO ~prio 79, CPU-pinned, woken every slot period by
 * clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) on an absolute deadline that
 * advances by period_ns each tick (no drift accumulation).
 *
 * The graph thread produces whole 1492-B downstream frames (audio already encoded
 * by reac_tx_build) into a lock-free SPSC frame ring; the pacer consumes exactly
 * one per slot. On underrun (the graph fell behind) the pacer emits a silent
 * FILLER so the cadence — and the master heartbeat/channel-map, and the free-
 * running counter — never stall, which is what keeps a slaved box locked without
 * clicks. The pacer asks reac_master what control block each outgoing frame
 * carries and stamps it (FILLER audio / probe / grant / chanmap / cfea announce),
 * so the master establishment sequence rides the 8000 fps stream in-band.
 */
#ifndef REAC_PACER_H
#define REAC_PACER_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>

#include "reac_master.h"

struct reac_box_model;   /* reac_ctrl.h — master-side box recognition */

/* Lock-free SPSC ring of fixed-size frame slots (the TX equivalent of reac_ring,
 * but carrying whole encoded frames not planar samples). Producer = PipeWire
 * process(); consumer = the pacer thread. */
struct reac_frame_ring {
	uint8_t *buf;              /* slots * slot_sz bytes */
	uint16_t *len;            /* per-slot byte length */
	uint32_t slots;           /* power of two */
	uint32_t mask;
	uint32_t slot_sz;         /* >= REAC_FRAME_BYTES */
	_Atomic uint32_t head;    /* producer writes here */
	_Atomic uint32_t tail;    /* consumer reads here */
	_Atomic uint64_t overruns;  /* producer: frames dropped (ring full) */
	_Atomic uint64_t underruns; /* consumer: slots with no frame ready */
};

int  reac_frame_ring_init(struct reac_frame_ring *r, uint32_t slots, uint32_t slot_sz);
void reac_frame_ring_free(struct reac_frame_ring *r);
/* PRODUCER: copy one frame in. Drops (overrun) the NEWEST if full — SPSC forbids
 * the producer moving tail. Returns 1 on write, 0 on drop. */
int  reac_frame_ring_push(struct reac_frame_ring *r, const uint8_t *frame, uint16_t n);
/* CONSUMER: copy the oldest frame into `out` (slot_sz). Returns its length, or 0
 * if empty (underrun). REALTIME-SAFE. */
uint16_t reac_frame_ring_pop(struct reac_frame_ring *r, uint8_t *out);
uint32_t reac_frame_ring_readable(const struct reac_frame_ring *r);

/* ---- drain-to-target depth guard (task #152) ----------------------------- *
 * The graph pushes whole encoded frames into the ring; the pacer drains exactly
 * one per slot. If the graph clock runs marginally fast versus the wire clock,
 * the ring depth walks up over long uptime toward the ~250 ms ring cap — silently
 * inflating graph->wire latency. As the ring's CONSUMER (it alone owns tail) the
 * pacer drops the OLDEST excess back to TARGET whenever the depth exceeds HIGH,
 * bounding that walk. SPSC-safe.
 *
 * The band is derived from the ACTUAL producer burst, NOT a guessed steady state.
 * The graph delivers up to one quantum of audio per process() callback, i.e.
 * quantum/12 frames pushed at once, so the ring depth SAWTOOTHS by that much
 * every drive cycle — measured LIVE at ~38..113 frames (peak 113) with openmixer
 * feeding playback_08 at the deployed quantum (an earlier guessed HIGH of 128
 * sat one bigger burst away from trimming healthy audio). HIGH must clear several
 * such bursts: HIGH = max(FLOOR, MULT * quantum_frames), TARGET = HIGH/2. FLOOR
 * is a hard minimum well above the live 113-frame peak — 512 frames ~= 128 ms
 * @48k, half the 256 ms cap, so normal operation NEVER trims and only genuine
 * runaway drift toward the cap does. quantum_frames is plumbed live from the sink
 * node's process() (graph_quantum); 0 before the first callback -> FLOOR. */
#define REAC_PACER_GUARD_FLOOR_FRAMES   512u   /* hard min HIGH (~128 ms @48k)     */
#define REAC_PACER_GUARD_BURST_MULT       4u   /* ring holds >= this many bursts   */

/* HIGH watermark for a given producer burst size (frames per process() callback,
 * = quantum/12). PURE, unit-tested: max(FLOOR, MULT * quantum_frames). TARGET is
 * always HIGH/2. */
uint32_t reac_pacer_guard_high(uint32_t quantum_frames);

/* Depth-telemetry heartbeat (the master is a long-lived systemd service logging
 * to journald). The depth SAWTOOTHS with each producer burst, so a per-change
 * band would fire every drain — the line is emitted ONLY on this heartbeat
 * (carrying the interval min/max/last depth) or when the guard trims. */
#define REAC_PACER_DEPTH_LOG_HB_NS     10000000000ull  /* 10 s health heartbeat */

/* Depth-guard math (PURE — unit-tested): the number of frames to drop so a ring
 * of `depth` frames drains back to `target`, but only once `depth` exceeds the
 * `high` watermark. At/below `high` returns 0 (normal jitter is never trimmed);
 * a `target` >= `depth` also returns 0 (defensive, never an underflowing drop). */
uint32_t reac_frame_ring_trim_count(uint32_t depth, uint32_t high, uint32_t target);

/* CONSUMER-side trim: drop the OLDEST frames so the depth returns to `target`,
 * but only when it exceeds `high` (reac_frame_ring_trim_count). SPSC-safe — only
 * the consumer (the pacer thread) moves tail. Returns frames dropped. RT-SAFE. */
uint32_t reac_frame_ring_trim(struct reac_frame_ring *r, uint32_t high, uint32_t target);

/* ---- the FSM event log (SPSC ring, RT-safe producer) ----------------------
 * The pacer thread is SCHED_FIFO: no stdio there. It pushes fixed-size events
 * into a small lock-free SPSC ring (drop-newest on full, like the frame ring);
 * the PipeWire main loop drains + formats them (reac_pacer_log_drain) so the
 * next live power-cycle prints a complete establishment transcript. */
enum reac_pacer_evkind {
	REAC_PEV_STATE = 0,      /* FSM transition: a=from, b=to, blk[0]=cause     */
	REAC_PEV_RX,             /* rx control seen (rate-limited): a=ev, b=state  */
	REAC_PEV_JOIN,           /* box JOIN: a=broadcast?, b=new state, blk=block */
	REAC_PEV_PRESENCE,       /* presence edge: a=gained(1)/lost(0)             */
	REAC_PEV_GRANT_TIMEOUT,  /* grant window expired: a=attempt# (mod 256)     */
	REAC_PEV_DROP,           /* backward drop: a=reason, blk=block for BYE     */
	REAC_PEV_WATCHDOG,       /* still PROBING after 10 s: a=box_seen           */
	REAC_PEV_RECOGNIZED,     /* box model recognized: a=in_ch (matrix lookup)  */
};

/* Cause codes for REAC_PEV_STATE blk[0]: 0..3 = the reac_master_rx_event that
 * fired the transition; REAC_PEV_CAUSE_TIMER = a safety-fallback timer. */
#define REAC_PEV_CAUSE_TIMER 0xff

struct reac_pacer_event {
	uint64_t mono_ns;
	uint8_t  kind;           /* enum reac_pacer_evkind */
	uint8_t  a, b;
	uint8_t  src[6];         /* the box MAC involved (zero if n/a) */
	uint8_t  blk[32];        /* raw control block for JOIN/BYE; cause for STATE */
};

#define REAC_PACER_EVRING 128    /* power of two */

/* Per-slot bounded non-blocking RX drain budget (8x wire-rate headroom per
 * 125 us slot; the box floods <=1 frame/slot on average). */
#define REAC_PACER_RX_BUDGET 8

struct reac_pacer_cfg {
	const char *ifname;   /* TX NIC (raw AF_PACKET 0x8819) */
	int fps;              /* slot cadence: 3675 / 4000 / 8000 */
	int prio;             /* SCHED_FIFO priority (0 -> default 79) */
	int cpu;              /* CPU to pin to (<0 -> no affinity) */
	const uint8_t *src_mac;   /* our master MAC (Roland OUI); NULL -> a stand-in */
	struct reac_console_cfg console;  /* box I/O advertised downstream; a zero
	                                   * out_channels -> the S-1608 default */
};

struct reac_pacer {
	struct reac_frame_ring ring;     /* graph -> pacer */
	struct reac_master master;       /* the establishment state machine */
	int fd;                          /* AF_PACKET socket */
	int ifindex;
	long period_ns;                  /* 1e9 / fps */
	int prio, cpu;
	uint8_t src[6];

	pthread_t thread;
	_Atomic int running;
	_Atomic int started;             /* thread reached its RT loop */

	/* diagnostics (read from any thread) */
	_Atomic uint64_t tx_frames;
	_Atomic uint64_t tx_errors;
	_Atomic uint64_t late_wakes;     /* slots where we woke > 1 period late */

	/* frame-ring depth guard telemetry (task #152). ring_depth_{min,peak} bound the
	 * depth SAWTOOTH seen since the last EMITTED depth line (read-and-reset there),
	 * so a heartbeat surfaces the interval's real min/max; the trim counters are
	 * cumulative. Written by the pacer thread, read+reset by the non-RT drain.
	 * graph_quantum is the latest process() quantum (samples), plumbed live from
	 * the sink node so the guard band tracks the actual producer burst. */
	_Atomic uint64_t ring_trims;        /* times the depth guard fired */
	_Atomic uint64_t ring_trim_frames;  /* total frames the guard dropped */
	_Atomic uint32_t ring_depth_peak;   /* max ring depth since last emitted line */
	_Atomic uint32_t ring_depth_min;    /* min ring depth since last emitted line */
	_Atomic uint32_t graph_quantum;     /* latest graph quantum in samples (0 = none) */

	/* RX / establishment diagnostics (written by the pacer thread only) */
	_Atomic int      fsm_state;      /* mirror of master.state for cross-thread reads */
	_Atomic uint64_t rx_box_frames;  /* classified box frames (incl. FILLER) */
	_Atomic uint64_t rx_box_ctrl;    /* classified box CONTROL frames */
	_Atomic uint64_t rx_joins;       /* validated JOINs seen */
	/* Written only by the pacer thread (dedup for the RECOGNIZED pev); _Atomic
	 * so a non-RT reader (the reac.box-model / reac.box-width property poll)
	 * can load it from another thread without a data race. A pointer store/load
	 * is lock-free on every arch reac-pw targets. */
	_Atomic (const struct reac_box_model *) recognized_box;
	_Atomic uint64_t grant_attempts; /* grant windows opened */
	_Atomic uint64_t drops[8];       /* backward drops by reac_master_drop_reason */

	/* FSM event log ring (producer = pacer thread, consumer = main loop) */
	struct reac_pacer_event evring[REAC_PACER_EVRING];
	_Atomic uint32_t ev_head, ev_tail;   /* free-running u32 indices */
	_Atomic uint64_t ev_drops;           /* events dropped (ring full) */

	/* pacer-thread-local bookkeeping (single-writer, no atomics needed) */
	int      fps;                    /* slot cadence (for the 10 s watchdog) */
	enum reac_master_state prev_state;  /* to detect next()-driven transitions */
	int      presence_seen;          /* last logged m.box_seen edge */
	uint32_t rx_since_change[4];     /* per-rx-event counts since a transition */
	uint64_t probing_slots;          /* fruitless-probing watchdog counter */
	uint64_t last_chanmap_ns;        /* for heartbeat-after-walk latency */

	/* depth-telemetry drain state (NON-RT drain / main-loop thread only; single
	 * writer, no atomics needed) — gates the depth line to the heartbeat + trims
	 * (see REAC_PACER_DEPTH_LOG_HB_NS). Zero-initialised, so the first drain emits
	 * one baseline line via the heartbeat branch. */
	uint64_t log_last_ns;            /* mono_ns of the last emitted depth line */
	uint64_t log_last_trims;         /* ring_trims count at the last depth line */
};

/* period for an fps (ns). Exposed for the unit test. */
long reac_pacer_period_ns(int fps);

/* Open the TX socket + size the frame ring (~250 ms deep). Does NOT start the
 * thread or touch scheduling. Returns 0 / -1. */
int  reac_pacer_open(struct reac_pacer *p, const struct reac_pacer_cfg *cfg);

/* Spawn the SCHED_FIFO pacer thread (mlockall + affinity + sched_setscheduler are
 * done inside the thread). Returns 0 / -1. */
int  reac_pacer_start(struct reac_pacer *p);

/* PRODUCER side (call from the graph thread): hand one encoded downstream frame
 * to the pacer. Returns 1 if queued, 0 if dropped (ring full). */
int  reac_pacer_submit(struct reac_pacer *p, const uint8_t *frame, uint16_t n);

/* Classify one received raw frame and feed the master FSM (PACER THREAD ONLY —
 * it owns struct reac_master). Takes no socket, so it is unit-testable: bumps
 * the rx_* counters, mirrors fsm_state, and pushes log events into the ring.
 * The pacer's per-slot drain calls this for every recv()'d frame. */
void reac_pacer_rx_ingest(struct reac_pacer *p, const uint8_t *frame, size_t len);

/* CONSUMER side (any non-RT thread, e.g. a 200 ms main-loop timer): drain the
 * event ring, formatting each event to `out` (one line per event). Returns the
 * number of events drained. */
int  reac_pacer_log_drain(struct reac_pacer *p, FILE *out);

void reac_pacer_stop(struct reac_pacer *p);
void reac_pacer_close(struct reac_pacer *p);

#endif /* REAC_PACER_H */
