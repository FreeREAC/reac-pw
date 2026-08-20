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
 * by libreac's reac_downstream_build) into a lock-free SPSC frame ring; the pacer consumes exactly
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
#include "reac_disco.h"
#include "reac_headamp_tx.h"
#include "reac_clock.h"

struct reac_box_model;   /* reac_ctrl.h — master-side box recognition */

/* THE FRAME DOUBLING (issue #92). Every real desk transmits every downstream
 * frame TWICE, back-to-back, with the SAME counter — measured across the
 * 2026-07 goldens: the M-200 @48k alternates counter deltas 0,1,0,1 through
 * its whole full-rate stream (8000 pps wire = 4000 unique fps — which also
 * reconciles libreac's REAC_MODE_48K with the captured pace), and the M-5000
 * @96k doubles every sparse control emission while hunting AND established
 * (delta 0 = half of all deltas; counter free-running at 8000/s underneath).
 * Boxes are single-emission upstream, so this is a DOWNSTREAM property, and
 * receivers dedup by counter (ours does: reac_rx_dup). A single-emission
 * master is distinguishable from every real desk on the wire — and it is the
 * one measured difference in the S-4000 join refusal (2026-08-20 logs).
 * Per-slot wire pps is therefore fps * REAC_PACER_TX_REPS. */
#define REAC_PACER_TX_REPS 2

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
	REAC_PEV_SIGHTING,       /* passive discovery: a=role, b=model idx+1 (0=?) */
	REAC_PEV_CLOCK,          /* clock discipline changed (#75): a=reac_clock_source,
	                          * b=reac_clock_state | (reac_clock_quality << 4)
	                          * (#77 — both enums are <16, and blk is full: 4 bytes
	                          * of ppm + a 28-byte label leaves no room), blk[0..3]=
	                          * applied ppm*1000 LE int32, blk[4..]=device label.
	                          * ONLY ever pushed when following is ENABLED —
	                          * with the knob unset the transcript is unchanged.   */
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

/* Live head-amp control command ring (task #203). A controller (openmixer) sets a
 * per-channel phantom/pad/sens prop on the master node; the PipeWire main-loop
 * prop handler enqueues the change here and the RT pacer thread drains + applies
 * it. SPSC, opposite direction from the event ring: PRODUCER = main loop,
 * CONSUMER = the pacer thread. Each entry is a reac_headamp_pack()'d triple, so a
 * whole (ch,param,value) command is one atomic word and the RT reader can never
 * see a torn triple. 64 slots absorbs a controller pushing a full desk's worth of
 * head-amp state in one gesture faster than the pacer drains one per slot. */
#define REAC_HEADAMP_CMD_RING 128  /* power of two */

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
	/* Optional MASTER head-amp send table (task #155): the operator's per-channel
	 * phantom/pad/sens, loaded into the DMX re-assert scheduler at open. NULL/0 ->
	 * the head-amp overlay is entirely off and the downstream is unchanged. */
	const struct reac_headamp_setting *headamps;
	int n_headamps;
	/* Clock discipline (#75). 0 (the default) = the pacer advances its deadline by
	 * the fixed nominal period exactly as it always has — no discipline object is
	 * consulted, no reference is read, and the emission is byte- and
	 * timing-identical. 1 = follow the best available reference. Set from
	 * REACPW_CLOCK_FOLLOW; see docs/ENV-KNOBS.md. */
	int clock_follow;
};

/* Re-evaluate the clock discipline every this many slots (~8 Hz at 8000 fps).
 * reac_rx recomputes its slope ~4x/s, so anything faster only re-reads the same
 * estimate; anything slower makes a vanished reference take too long to notice. */
#define REAC_CLOCK_TICK_SLOTS   1024u

/* A reference whose publisher has been silent this long has VANISHED, whatever
 * its presence bit still says — a publisher that dies must not leave us claiming
 * lock to a corpse. */
#define REAC_CLOCK_STALE_NS     2000000000ull

/* An operator needs to see WHICH device is being followed, not just which tier:
 * "locked to graph clock (RME Babyface Pro)" and "locked to box counter slope
 * (S-1608)" are actionable, "locked to graph clock" is not. Each reference
 * therefore carries a publisher-owned label. 28 bytes so it rides in the event
 * ring's existing 32-byte block alongside the applied ppm; longer names truncate.
 *
 * The label is written rarely (only when it CHANGES) from the publisher's thread
 * — which for the graph reference is the RT graph thread — and read by the pacer
 * thread at the tick rate. A two-phase sequence makes a torn copy DETECTABLE:
 * an odd seq, or a seq that moved across the copy, means "retry", never "print
 * half a name". No lock, no allocation, RT-safe. */
#define REAC_CLOCK_LABEL_MAX 28

struct reac_clock_label {
	_Atomic uint32_t seq;              /* odd = a write is in progress */
	char name[REAC_CLOCK_LABEL_MAX];
};

/* Single-publisher per label. Returns 1 if the label actually changed (so a
 * per-quantum caller costs one strncmp and nothing else). */
int reac_clock_label_set(struct reac_clock_label *l, const char *name);
/* Copy the label out. Returns 1 on a clean read, 0 if it was mid-write (the
 * caller prints nothing rather than a torn name). */
int reac_clock_label_get(const struct reac_clock_label *l, char *out, size_t cap);

struct reac_pacer {
	struct reac_frame_ring ring;     /* graph -> pacer */
	struct reac_master master;       /* the establishment state machine */
	struct reac_headamp_tx headamp;  /* MASTER head-amp DMX send (off unless set) */
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

	/* Passive discovery (task #178). Two halves, deliberately on opposite sides of the
	 * event ring so no new cross-thread primitive is needed:
	 *   disco_gate — PACER-THREAD-ONLY. Decides which sightings earn a ring slot.
	 *   disco      — MAIN-THREAD-ONLY. Built by reac_pacer_log_drain from the ring and
	 *                read by the sink node's property poll. The pacer thread must never
	 *                touch it.
	 * See reac_disco.h; the seam it feeds is documented in openmixer's
	 * docs/design/specs/2026-07-16-reac-discovery-via-reac-pw.md. */
	struct reac_disco_gate disco_gate;
	struct reac_disco_table disco;

	/* FSM event log ring (producer = pacer thread, consumer = main loop) */
	struct reac_pacer_event evring[REAC_PACER_EVRING];
	_Atomic uint32_t ev_head, ev_tail;   /* free-running u32 indices */
	_Atomic uint64_t ev_drops;           /* events dropped (ring full) */

	/* Live head-amp command ring (task #203): producer = PipeWire main loop
	 * (reac_pacer_headamp_set), consumer = the RT pacer thread
	 * (reac_pacer_headamp_drain). Each cell is a reac_headamp_pack()'d
	 * (ch,param,value) — one atomic word, no torn triple. The pacer thread
	 * remains the SOLE writer of the head-amp TABLE (it applies drained commands
	 * via reac_headamp_tx_set), so the emit path (reac_headamp_tx_next) needs no
	 * lock and stays RT-safe. */
	_Atomic uint32_t ha_cmd[REAC_HEADAMP_CMD_RING];
	_Atomic uint32_t ha_cmd_head, ha_cmd_tail;  /* free-running u32 indices */
	_Atomic uint64_t ha_cmd_drops;              /* commands dropped (ring full) */
	_Atomic uint64_t ha_cmd_applied;            /* commands drained + applied (diag) */

	/* ---- clock discipline (#75) ------------------------------------------- *
	 * INERT unless clock_follow is set. Publishers (any thread) drop a ppm sample
	 * into their OWN slot — one slot per source, so two references can never race
	 * over a single word — and set/clear their presence bit. The pacer thread owns
	 * `clock` and is the only reader; it selects the best available reference,
	 * feeds the DLL, and takes the steered period from it.
	 *
	 * The pacer is the MASTER path by definition (the slave path runs no pacer at
	 * all — frame arrival is its slot clock), so the discipline is initialised with
	 * the master hierarchy: PHC > hardware-driven graph clock > box counter slope >
	 * free-run. Generating the pace does not make us the clock master. */
	int clock_follow;                                  /* the knob, read-only after open */
	_Atomic uint32_t clock_present;                    /* availability bitmap  */
	_Atomic int      clock_ppm_milli[REAC_CLOCK_SRC_COUNT];  /* ppm * 1000     */
	_Atomic uint64_t clock_stamp_ns[REAC_CLOCK_SRC_COUNT];   /* last publish   */
	struct reac_clock_label clock_label[REAC_CLOCK_SRC_COUNT];  /* which device */
	/* What the PUBLISHER inferred about each reference from its device name and
	 * the operator's designation (#77). The pacer thread only ranks — it never
	 * parses a name — so this is one relaxed int per source, published beside the
	 * sample it describes. Zero-initialised to UNUSABLE is fine because a source
	 * with no publisher has no availability bit either. */
	_Atomic int      clock_quality[REAC_CLOCK_SRC_COUNT];    /* reac_clock_quality */

	/* pacer-thread-local bookkeeping (single-writer, no atomics needed) */
	struct reac_clock_disc clock;    /* PACER THREAD ONLY */
	long     slot_period_ns;         /* the steered period actually slept to */
	uint32_t clock_slots;            /* slots since the last re-evaluation */
	uint32_t clock_gen_seen;         /* last reported discipline generation */
	uint64_t clock_stamp_seen[REAC_CLOCK_SRC_COUNT];  /* freshness per source */
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

/* LIVE head-amp control (task #203). PRODUCER side — call from the PipeWire main
 * loop (the node's prop handler). Enqueues one per-channel (ch,param,value)
 * change for the RT pacer thread to apply to the head-amp send table, so an
 * external controller can drive the box preamps at runtime (mixer knob -> prop ->
 * real box). Master-role only (a slave never SENDS head-amp). Non-blocking and
 * lock-free: it packs the triple into one atomic word and pushes it into the
 * SPSC command ring — no syscall, no lock, safe to call from the loop thread.
 * Returns 1 if queued, 0 if the ring was full (the change is dropped; the DMX
 * re-assert would carry a later value anyway). */
int  reac_pacer_headamp_set(struct reac_pacer *p, uint8_t ch, uint8_t param,
                            uint8_t value);

/* CONSUMER side — drain every queued live head-amp command and apply it to the
 * send table via reac_headamp_tx_set (arming the table + marking each cell dirty
 * so the change goes out as an edge, then re-asserts). The RT pacer thread calls
 * this once per slot; it is exposed so the offline test can drive the same
 * apply path without the RT thread. Returns the number of commands applied.
 * PACER-THREAD-ONLY in production (it is the sole writer of the head-amp table). */
int  reac_pacer_headamp_drain(struct reac_pacer *p);

/* CLOCK_MONOTONIC in ns — the one clock every reac.discovery.* timestamp is measured
 * against (sighting, staleness aging, and the published age_ms). */
uint64_t reac_pacer_mono_ns(void);

/* ---- clock discipline (#75) ---------------------------------------------- */

/* PUBLISH one clock-reference sample. Callable from ANY thread (the graph's RT
 * process(), the RX feeder, a main-loop poll): each source owns its own slot, so
 * publishers never race each other, and a store is three relaxed atomics — no
 * lock, no syscall, RT-safe. `present` clears the source's availability bit when
 * 0, which is how a publisher says its reference went away; a publisher that
 * simply stops is also caught, by REAC_CLOCK_STALE_NS.
 *
 * `label` names the actual device behind this reference (the graph driver's clock
 * name, the recognized box, ...) so the transcript is actionable; NULL leaves the
 * current label alone.
 *
 * `quality` is what the publisher INFERRED about that device (#77) — it is the
 * publisher that holds the name and the operator's designation, and the pacer
 * thread that ranks. Pass REAC_CLOCK_Q_UNGRADED to express no opinion; that is
 * the pre-#77 behaviour exactly.
 *
 * Publishing while clock following is DISABLED is harmless and changes nothing —
 * the pacer never reads these slots. */
void reac_pacer_clock_publish(struct reac_pacer *p, enum reac_clock_source src,
                              int present, int ppm_milli, const char *label,
                              enum reac_clock_quality quality, uint64_t now_ns);

/* PACER THREAD ONLY. Re-evaluate the discipline (rate-limited internally to one
 * evaluation per REAC_CLOCK_TICK_SLOTS) and return the period this slot should
 * advance the deadline by.
 *
 * INERT: with clock_follow == 0 this returns p->period_ns — the same constant the
 * pacer has always used — and touches nothing else at all. Exposed so the offline
 * test can drive the whole path (including proving inertness) without the RT
 * thread or a socket. */
long reac_pacer_clock_tick(struct reac_pacer *p, uint64_t now_ns);

void reac_pacer_stop(struct reac_pacer *p);
void reac_pacer_close(struct reac_pacer *p);

#endif /* REAC_PACER_H */
