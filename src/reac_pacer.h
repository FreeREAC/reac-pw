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
 * pattern: mlockall, SCHED_FIFO in the wire-clock band BELOW the PipeWire graph
 * (reac_rt.h; REACPW_RT_PRIO to move it), CPU-pinned, woken every slot period by
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

#include <net/if.h>   /* IFNAMSIZ */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>

#include "reac_master.h"
#include "reac_disco.h"
#include "reac_headamp_tx.h"
#include "reac_clock.h"
#include "reac_rate_cfg.h"
#include "reac_rt.h"
#include <reac/reac_identity.h>   /* the box identity-page decode (DT1 tag 0x0500) */

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
/* The frame ring's slot size. Every buffer a pop can land in must be at least
 * this big — the pacer emits straight out of the popped buffer, so the slot
 * width, not the frame width, is what bounds the write. */
#define REAC_PACER_SLOT_SZ 2048u

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

/* The floor, overridable at runtime by REACPW_GUARD_FLOOR_FRAMES.
 *
 * The fast-path spec's M5 -- sweep this constant down until tx_errors, filler
 * frames or trims appear -- was the highest-value measurement it named, and it
 * was also the only one of M1..M5 that demanded a rebuild of reac-pw between
 * steps, which is what made it the only one needing a rig restart. As an
 * environment variable the sweep is a restart of the daemon per step and nothing
 * more. Out-of-range or unparseable values keep the compiled floor: a sweep must
 * never be able to silently configure a ring too shallow to hold one producer
 * burst, because the symptom of that is filler frames, which sound like nothing
 * in particular. Read ONCE, off the RT path. */
uint32_t reac_pacer_guard_floor(void);

/* Bounds for the override. The low bound is one 1024-sample quantum's worth of
 * frames (1024/12 = 85) rounded up to 96 -- below that the ring cannot absorb a
 * single producer burst and every drive cycle underruns. */
#define REAC_PACER_GUARD_FLOOR_MIN       96u
#define REAC_PACER_GUARD_FLOOR_MAX     4096u
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

/* The discard-rate detector's window and how many consecutive windows must have
 * discarded audio before it speaks. 30 s x 3 = a minute and a half of continuous
 * loss before the warning, which is long enough that a scene recall or a startup
 * transient cannot trip it and short enough to be on screen before a show. */
#define REAC_PACER_DISCARD_WIN_NS      30000000000ull  /* 30 s discard window   */
#define REAC_PACER_DISCARD_WIN_RUN     3u              /* windows before warning */

/* The discard-rate detector's state and decision, split out PURE so it can be
 * unit-tested. It is split out because the version it replaces was not testable
 * and was not tested, and was therefore wrong in the field for as long as it
 * existed: it required twenty CONSECUTIVE trimming drains, the rig trims once
 * every ~24 s against a 200 ms drain cadence, and so it never fired once while
 * 64 ms of audio went missing every 24 seconds. A detector nobody can drive from
 * a test is a detector nobody has checked. */
struct reac_discard_watch {
	uint64_t win_ns;      /* mono_ns the current window opened (0 = not started) */
	uint64_t win_frames;  /* cumulative trim_frames when it opened               */
	unsigned run;         /* consecutive closed windows that discarded audio     */
	int      warned;      /* the warning has been emitted (once per run)         */
};

/* Advance the watch. `trim_frames` is the pacer's cumulative discarded-frame
 * count. Returns 1 EXACTLY ONCE, on the window that completes a run of
 * REAC_PACER_DISCARD_WIN_RUN windows each of which discarded at least one frame;
 * 0 otherwise. On a 1 return, *frames_per_s carries the rate measured over the
 * window that triggered it. PURE apart from the struct it is handed. */
int reac_discard_watch_step(struct reac_discard_watch *w, uint64_t now,
                            uint64_t trim_frames, double *frames_per_s);

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
	REAC_PEV_RECOGNIZED,     /* box declared its geometry: a=in_ch (declared),
	                          * b=matrix model index+1 (0 = no row names it)   */
	REAC_PEV_SIGHTING,       /* passive discovery: a=role, b=model idx+1 (0=?),
	                          * blk[0]=the frame's channel GEOMETRY (0 = none legal).
	                          * The width is not decoration here: arbitration decides a
	                          * DESK from a stagebox strapped to master by geometry alone
	                          * (reac_arbitration.h §2b), and until 2026-09-09 it never
	                          * crossed this ring — so the master side published every
	                          * such rival as `unknown` and could not refuse one.        */
	REAC_PEV_CLOCK,          /* clock discipline changed (#75): a=reac_clock_source,
	                          * b=reac_clock_state | (reac_clock_quality << 4)
	                          * (#77 — both enums are <16, and blk is full: 4 bytes
	                          * of ppm + a 28-byte label leaves no room), blk[0..3]=
	                          * applied ppm*1000 LE int32, blk[4..]=device label.
	                          * ONLY ever pushed when following is ENABLED —
	                          * with the knob unset the transcript is unchanged.   */
};

/* Cause codes for REAC_PEV_STATE blk[0]: 0..3 = the reac_master_rx_event that
 * fired the transition; REAC_PEV_CAUSE_TIMER = a safety-fallback timer;
 * REAC_PEV_CAUSE_RATE_CHANGE = an accepted `reac.cfg.rate` re-establish
 * (reac_pacer_apply_rate), so the transcript tells a rate change apart from
 * an ordinary drop.
 *
 * LINK_UP / LINK_DOWN are the same re-establish driven by the CABLE (#95). They
 * are distinct codes because the three read completely differently to an
 * operator: a rate change is something a controller asked for, a link edge is
 * something the room did, and a plain drop is the box's own doing. A transcript
 * that called all of them "rate change" would send a cable fault to whoever
 * touched the console last. */
#define REAC_PEV_CAUSE_TIMER        0xff
#define REAC_PEV_CAUSE_RATE_CHANGE  0xfe
#define REAC_PEV_CAUSE_LINK_UP      0xfd
#define REAC_PEV_CAUSE_LINK_DOWN    0xfc

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
	int prio;             /* SCHED_FIFO priority; 0 -> resolved by reac_rt.h
                       * (REACPW_RT_PRIO, else the built-in that sits
                       * BELOW the PipeWire graph) */
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
	/* SLOT-DEBT CATCH-UP (workstream CLK, 2026-08-23). How many overslept slots the
	 * pacer will repay by staying on its original deadline grid instead of
	 * re-basing the phase to `now`. 0 = the rate-derived default
	 * (reac_catchup_default_slots — a zero-initialised cfg gets the default,
	 * never "off"); -1 restores the historical behaviour, re-base always and lose
	 * the overslept slots for good; >0 is an explicit slot count, which is
	 * RATE-DEPENDENT and exists for sweeps. Set from REACPW_CATCHUP_MAX_SLOTS;
	 * see docs/ENV-KNOBS.md. */
	int catchup_max_slots;
	/* Drivability (2026-08-26-reac-runtime-config.md §0): which of the closed
	 * three rates (reac_rate_cfg.h) this segment can actually be driven at. 0
	 * (a zero-initialised cfg) means REAC_RATE_ALL_BITS — the honest default
	 * for a daemon with no real probe: declare the whole closed list drivable
	 * rather than guess a narrower one. A caller with an actual measurement
	 * (today: only a test, standing in for the probe that does not exist yet)
	 * passes a narrower mask directly. */
	unsigned drivable_mask;
	/* 1 when the opening rate was ASSERTED (--rate, a conf file) rather than picked
	 * by the best-drivable convention — the source label starts truthful either way. */
	int rate_asserted;
};

/* Default slot-debt budget, EXPRESSED IN TIME because the thing it bounds is a
 * duration and not a slot count.
 *
 * THE PACER OVERSLEEPS BY AN AMOUNT OF TIME. It is a scheduler tail — a
 * preemption, an interrupt, a stall — and it has no idea what the REAC rate is.
 * So a budget written as "4 slots" silently means 1.0 ms at 48 kHz and 0.5 ms at
 * 96 kHz: the SAME hiccup that is repayable on one rig becomes unrepayable on the
 * other, and nothing says so. That is not a tuning question, it is a units bug
 * waiting for a rate change, and this rig has a rate change coming.
 *
 * 1000 us is MEASURED, not guessed. From the 30-minute soak's per-window worst
 * single debt (198 windows, `reac.health.slot-debt-max`):
 *
 *     p50 250 us   p90 500 us   p95 750 us   worst 2000 us
 *
 * 1.0 ms covers ~97% of what a half-hour throws at a busy host; the remainder is
 * real stalls, which are reported rather than smeared onto the wire. It bounds
 * the catch-up burst to 1.0 ms of frames — 4 x 1492 B = 48 us of wire at 48 kHz,
 * 8 x 1492 B = 95 us at 96 kHz, both a small fraction of a slot and both well
 * under the 10.8 us stddev the box's OWN return already carries.
 *
 * AT 48 kHz THIS IS EXACTLY THE 4 SLOTS THAT WERE SOAKED — 1000 us x 4000 fps /
 * 1e6 = 4, by construction — so the shipping behaviour is unchanged and the
 * measurement behind it still applies. At 96 kHz it becomes 8, which is the same
 * duration and is the number the 48 kHz distribution implies. That does NOT make
 * 96 kHz verified; see docs/96K-SWITCH-ASSESSMENT.md. It makes the default stop
 * being wrong for a reason nobody would have seen. */
#define REAC_CATCHUP_MAX_DEFAULT_US 1000

/* The budget in slots for a given frame rate. Rounds to at least 1: a budget that
 * rounds to zero would silently disable catch-up at an absurd rate, and "off"
 * must be something a caller ASKS for. */
static inline uint32_t reac_catchup_default_slots(int fps)
{
	if (fps <= 0)
		return 1;
	uint32_t n = (uint32_t)(((double)REAC_CATCHUP_MAX_DEFAULT_US * (double)fps)
	                        / 1e6 + 0.5);
	return n ? n : 1;
}

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
	/* The TX NIC's name, COPIED at open (never the cfg's pointer: the caller's
	 * cfg need not outlive us). Only for diagnostics — asking the kernel whether
	 * this link still has carrier, which the watchdog reports. */
	char ifname[IFNAMSIZ];
	struct reac_frame_ring ring;     /* graph -> pacer */
	struct reac_master master;       /* the establishment state machine */
	/* Announced the instant a session is (re)established, so a consumer can drop
	 * per-session state at the seam instead of a housekeeping tick later — a tick
	 * late, the box's first frames of the new session are measured against the
	 * old session's counter and the seam is booked as lost frames (134 on a
	 * measured warm replug, where the correct answer is 0). A callback rather
	 * than a direct call so the pacer keeps knowing nothing about the receiver.
	 * Must be allocation-free and non-blocking: this fires on the RT path. */
	void  *session_ctx;
	void (*on_session)(void *ctx, const uint8_t mac[6], unsigned session);
	struct reac_headamp_tx headamp;  /* MASTER head-amp DMX send (off unless set) */
	int fd;                          /* AF_PACKET socket */
	int ifindex;
	long period_ns;                  /* 1e9 / fps */
	uint32_t catchup_max_slots;      /* slot-debt budget; 0 = re-base always */
	int      catchup_max_slots_cfg;  /* the RAW reac_pacer_cfg value that produced
	                                  * the line above (0 = rate-derived default,
	                                  * -1 = off, >0 = an explicit sweep count) —
	                                  * kept so a live rate change can re-resolve
	                                  * the budget for the new fps the same way
	                                  * reac_pacer_open did for the first one,
	                                  * rather than leaving a stale rate-derived
	                                  * number behind (see resolve_catchup_slots
	                                  * in reac_pacer.c). */
	int prio, cpu;
	enum reac_rt_prio_source prio_src;  /* which layer chose prio; reported
                                     * when the thread goes SCHED_FIFO */
	uint8_t src[6];

	pthread_t thread;
	_Atomic int running;
	_Atomic int started;             /* thread reached its RT loop */

	/* diagnostics (read from any thread) */
	_Atomic uint64_t tx_frames;
	_Atomic uint64_t tx_errors;
	_Atomic uint64_t late_wakes;     /* slots where we woke > 1 period late */
	/* SLOT DEBT (workstream CLK). A late wake used to re-base the deadline to
	 * `now`, which throws the overslept slots away permanently: measured on the
	 * live rig at 3.6 slots/s = 900 ppm of transmit deficit, which is the ENTIRE
	 * cause of the TX ring's growth and therefore of the guard's 64 ms discards.
	 * These two counters separate the debt we repaid from the debt we declared. */
	_Atomic uint64_t slots_catchup;  /* late wakes repaid by staying on the grid */
	_Atomic uint64_t slots_dropped;  /* slots abandoned: the debt exceeded the budget */
	/* THE TAIL, which is the thing a short run cannot show. slots_dropped says how
	 * much debt we abandoned; it does not say whether that was a hundred one-slot
	 * misses or one hundred-slot stall, and those are different faults with
	 * different fixes. This is the largest SINGLE debt seen since the last read —
	 * the number that says whether the catch-up budget is set right, and the only
	 * one that can distinguish a busy host from a stall. Read-and-reset by the
	 * health poll so a heartbeat reports its own window's worst case. */
	_Atomic uint32_t slot_debt_max;  /* largest single overslept debt, in slots */

	/* Health window state. MAIN-LOOP ONLY (reac_pacer_health_poll) — never touched
	 * by the pacer thread, so no atomics and no RT cost. */
	uint64_t health_win_ns;          /* monotonic ns at the window's open (0 = none) */
	uint64_t health_tx_frames;       /* tx_frames at the window's open */
	uint64_t health_trim_frames;     /* ring_trim_frames at the window's open */
	uint64_t health_late_wakes;
	uint64_t health_slots_dropped;
	uint64_t health_slots_catchup;

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
	/* THE ESTABLISHED BOX'S HEAD-AMP BASE, mirrored for cross-thread reads. It is
	 * the chassis strap the box announced (libreac reac_ports.h), carried here
	 * because it CANNOT be recomputed: it is not a function of the width, the
	 * model or anything else a reader already holds. The sink node used to derive
	 * it by calling the grant allocator on the recognized width, which worked only
	 * while width and strap were collinear on the three chassis we own.
	 * -1 means NO BOX IS ON THE WIRE — the same state recognized_box == NULL
	 * reports, not an established box whose base is unknown. */
	_Atomic int recognized_headamp_base;
	/* THE ENROLLED BOX'S OWN MAC, latched by the master from the source address of
	 * the JOIN it granted, mirrored here for cross-thread reads (reac.box.mac on
	 * the node). Packed into one word by reac_mac48_pack so it crosses as a single
	 * atomic — six loose bytes read from another thread is a torn read nobody
	 * synchronises, and half a MAC is a WRONG address rather than a stale one.
	 * 0 = NO BOX, which is what the master holds once it forgets one. */
	_Atomic uint64_t recognized_box_mac;
	/* THE BOX'S OWN IDENTITY, decoded from the identity-page replies (DT1 tag
	 * 0x0500) the grant sweep's group B polls. Written ONLY on the pacer thread as
	 * replies arrive (reac_pacer_rx_ingest); read by the non-RT property poll
	 * (reac.box-firmware / reac.box-hw) on another thread. A pointer/int atomic is
	 * not enough — the firmware, name and hw block do not fit one word — so the two
	 * cross a SEQLOCK: `identity_seq` is even when `rx_identity` is stable and odd
	 * while the writer is mid-update, and the reader retries until it reads the same
	 * even sequence on both sides. The writer is single (the pacer thread), so no
	 * writer lock is needed. Reset to "nothing seen" when the box is forgotten. */
	struct reac_identity rx_identity;
	_Atomic unsigned identity_seq;
	/* The geometry the box DECLARED (config-announce port table, libreac
	 * reac_ports_parse) and that reac_master_set_box last applied. Pacer-thread
	 * only (rx_ingest + sync_published_box run there): the dedup that stops the
	 * repeating config-announce from re-firing set_box every second. 0/0 =
	 * nothing declared (reset when the box is forgotten, so a re-declaration
	 * re-fires). The MODEL above only names; this is what sizes. */
	int declared_in, declared_out;
	_Atomic uint64_t grant_attempts; /* grant windows opened */
	_Atomic uint64_t drops[8];       /* backward drops by reac_master_drop_reason */

	/* Passive discovery (task #178). Two halves, deliberately on opposite sides of the
	 * event ring so no new cross-thread primitive is needed:
	 *   disco_gate — PACER-THREAD-ONLY. Decides which sightings earn a ring slot.
	 *   disco      — MAIN-THREAD-ONLY. Built by reac_pacer_log_drain from the ring and
	 *                read by the sink node's property poll. The pacer thread must never
	 *                touch it.
	 * See reac_disco.h; the seam it feeds is documented in openmixer's
	 * docs/design/specs/2026-07-16-reac-discovery-via-reac-pw.md.
	 *
	 * disco_peer_lock — PACER-THREAD-ONLY, like disco_gate. One reac_pacer is one segment
	 * (embedded in the segment's sink node), so its lifetime matches the segment's: a
	 * segment drop/reopen gets a fresh reac_pacer and so a fresh, unlocked lock — see
	 * reac_disco.h's header comment for what it defends against (the FILLER-frame gap
	 * left by removing the Roland-OUI check, 2026-09-03). */
	struct reac_disco_gate disco_gate;
	struct reac_disco_table disco;
	struct reac_disco_peer_lock disco_peer_lock;

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

	/* ---- live rate re-establish (2026-08-26-reac-runtime-config.md) --------- *
	 * A controller asserts `reac.cfg.rate` on the sink node's Props, same
	 * channel as head-amp. Deciding whether to accept it is PURE
	 * (reac_rate_cfg_decide) and happens on the caller's thread (the PipeWire
	 * main loop) before anything here is touched — a refused rate never
	 * reaches the pacer thread at all. What DOES need this thread is APPLYING
	 * an accepted one: p->period_ns, p->fps, the clock discipline and the
	 * whole `struct reac_master` (cycle_len, grant_dwell, ...: all fps-scaled,
	 * see reac_master_init) are pacer-thread-owned state, exactly like the
	 * head-amp TABLE above — so the handoff is the same single-writer pattern,
	 * simplified to one pending cell instead of a ring: unlike a head-amp
	 * write (one of up to REAC_HEADAMP_MAX_CH*3 independent cells), only the
	 * LATEST requested rate is ever meaningful, so a second assertion before
	 * the first drains simply supersedes it — nothing accumulates or replays
	 * out of order. rate_req_seq is bumped by the producer AFTER the value is
	 * stored (release), so the consumer's acquire load of the seq is what
	 * makes the stored value visible; rate_req_seen is PACER-THREAD-ONLY. */
	_Atomic int      rate_req_hz;
	_Atomic uint32_t rate_req_seq;
	uint32_t         rate_req_seen;             /* PACER THREAD ONLY */
	/* WHY the pending re-establish was asked for, as a REAC_PEV_CAUSE_* byte, so
	 * the transcript names the real reason. Stored BEFORE rate_req_seq is bumped,
	 * so the release/acquire pair that publishes the rate publishes this with it.
	 * 0 = the historical default (a rate change), which is what a caller that
	 * drives reac_pacer_apply_rate directly gets. */
	_Atomic int      reestab_cause;

	/* What is currently standing, for the property poll (sink_publish_link_
	 * props's rate-props analogue) to publish. Written by the pacer thread at
	 * open() and again whenever a rate request is applied; read from any
	 * thread once open() has returned. drivable_mask is set once at open() and
	 * never changes after (like p->clock_follow above) — plain, not atomic. */
	unsigned      drivable_mask;
	_Atomic int   rate_hz;             /* the standing rate, Hz               */
	_Atomic int   rate_asserted;       /* 0 = convention (best drivable), 1 = asserted */
	_Atomic int   rate_refused;        /* enum reac_rate_refuse, last refusal */
	_Atomic int   rate_reestablishing; /* 1 from an accepted request until the
	                                    * FSM reaches ESTABLISHED again        */

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
	struct reac_discard_watch discard;  /* sustained audio-discard detector     */
};

/* period for an fps (ns). Exposed for the unit test. */
long reac_pacer_period_ns(int fps);

/* Open the TX socket + size the frame ring (~250 ms deep). Does NOT start the
 * thread, but DOES resolve its priority (reac_rt.h reads config files, which the
 * RT thread must never do). Returns 0 / -1. */
int  reac_pacer_open(struct reac_pacer *p, const struct reac_pacer_cfg *cfg);

/* Spawn the pacer thread; it locks memory, pins itself and enters the wire-clock
 * SCHED_FIFO band (reac_rt_thread_go) as its first act. Returns 0 / -1. */
int  reac_pacer_start(struct reac_pacer *p);

/* PRODUCER side (call from the graph thread): hand one encoded downstream frame
 * to the pacer. Returns 1 if queued, 0 if dropped (ring full). */
int  reac_pacer_submit(struct reac_pacer *p, const uint8_t *frame, uint16_t n);

/* Classify one received raw frame and feed the master FSM (PACER THREAD ONLY —
 * it owns struct reac_master). Takes no socket, so it is unit-testable: bumps
 * the rx_* counters, mirrors fsm_state, and pushes log events into the ring.
 * The pacer's per-slot drain calls this for every recv()'d frame. */
void reac_pacer_rx_ingest(struct reac_pacer *p, const uint8_t *frame, size_t len);

/* Read the box's decoded identity (firmware / model / hw block) into *out, a
 * consistent snapshot lifted across the seqlock — safe from any non-RT thread
 * (the property poll). The result carries its own has_* flags: an address the box
 * never answered stays a fact, not a zero. Never blocks meaningfully — identity
 * writes happen only as a box establishes, so the reader reads a stable sequence
 * at once in practice. */
void reac_pacer_read_identity(const struct reac_pacer *p, struct reac_identity *out);

/* Read the ENROLLED BOX'S OWN MAC, packed by reac_mac48_pack — safe from any
 * non-RT thread (the property poll), one atomic load. 0 means NO BOX, the state
 * the master holds before a JOIN and again after it forgets one; pass it packed
 * to reac_box_mac_publish, which stamps exactly that as REAC_BOX_MAC_NONE. */
uint64_t reac_pacer_box_mac48(const struct reac_pacer *p);

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

/* PRODUCER side (task cfg.rate) — request that the pacer apply `hz` as the new
 * standing rate. The CALLER has already run it through reac_rate_cfg_decide
 * and gotten REAC_RATE_REFUSE_NONE back; this function does not re-validate,
 * it only hands the accepted value to the RT thread. Non-blocking, lock-free,
 * safe from the PipeWire main loop. Only the latest request matters (see the
 * struct's rate_req_seq comment), so there is no "ring full" case. */
void reac_pacer_request_rate(struct reac_pacer *p, int hz);

/* Request a re-establish AT THE STANDING RATE, attributed to `cause` (a
 * REAC_PEV_CAUSE_* byte). This is the SAME door as a rate change and lands in the
 * same reac_pacer_apply_rate — deliberately, because that function already is the
 * internal re-establish: it re-runs reac_master_init in place, ends the old
 * session, forgets the box and drops the recognizer, and the box re-enrols
 * through the identical grant/dwell sequence a cold start uses. A second path
 * that did the same thing for a cable instead of a rate would be a second FSM
 * re-entry to keep in step with this one, forever.
 *
 * Non-blocking and lock-free: safe from the PipeWire main loop, which is where
 * the link watcher runs (reac_linkmon is a socket read and is not RT-safe). */
void reac_pacer_request_reestablish(struct reac_pacer *p, int cause);

/* CONSUMER side — apply one already-accepted rate to `p`: recompute
 * period_ns/fps/the clock discipline/the catch-up budget for the new cadence,
 * then RE-ESTABLISH by re-running reac_master_init at the new fps (an
 * INTERNAL re-establish per the spec — no process restart, no new socket).
 * Re-init starts the FSM at IDLE exactly as reac_pacer_open originally did;
 * the very next pacer_loop iteration promotes it to PROBING and the box
 * re-enrolls through the same grant/dwell sequence as a cold start, just at
 * the new cadence. The console cfg and head-amp table are NOT reset — only
 * establishment is redone, per the ruling that a rate change re-clocks the
 * segment and nothing else. PACER-THREAD-ONLY in production; exposed so the
 * offline test can drive it without a live NIC. Returns the fps applied. */
int reac_pacer_apply_rate(struct reac_pacer *p, int hz);

/* Drain the latest pending rate request (if its seq is newer than what was
 * last applied) and apply it. The RT pacer thread calls this once per slot,
 * right beside reac_pacer_headamp_drain. Returns 1 if a rate was applied this
 * call, 0 if there was nothing new to apply. PACER-THREAD-ONLY in production;
 * exposed for the offline test. */
int reac_pacer_rate_drain(struct reac_pacer *p);

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

/* THE GRAPH-CLOCK SAMPLE, from whichever of our nodes the graph happens to drive.
 *
 * The admission and the grading used to live in the SINK's process callback, which made
 * the reference hostage to somebody having patched audio INTO the box's outputs: a
 * reac-playback node nobody has linked is SUSPENDED, its callback never runs, and the
 * daemon falls back to the box counter slope while an RME sits in the graph driving
 * everything else. Measured on the rig 2026-09-08 — both playback nodes suspended, zero
 * links, "free-running (no reference)" — with the capture node linked and running the
 * whole time. So the decision moved HERE and both nodes call it.
 *
 * `name` is the driver clock's name, `freewheel` its freewheeling flag, `rate_diff` its
 * speed as a ratio of CLOCK_MONOTONIC and `nsec` its timestamp. A freewheeling graph is
 * not a clock, and a software timer (clock.system.*) is our own free-run through a longer
 * pipe: both are refused here, which drops the pacer to the box slope or an honest
 * free-run. `clock_ref` is the operator's designation (REACPW_CLOCK_REF), which outranks
 * the name heuristic and is outranked in turn by measured stability.
 *
 * RT-SAFE and inert unless following: a handful of bounded scans over a 64-byte name plus
 * four relaxed atomics, no allocation, no syscall, and one predictable branch when the
 * discipline is off. Callable from any node's RT callback. */
void reac_pacer_clock_publish_graph(struct reac_pacer *p, const char *name, int freewheel,
                                    double rate_diff, uint64_t nsec, const char *clock_ref);

/* PACER THREAD ONLY. Re-evaluate the discipline (rate-limited internally to one
 * evaluation per REAC_CLOCK_TICK_SLOTS) and return the period this slot should
 * advance the deadline by.
 *
 * INERT: with clock_follow == 0 this returns p->period_ns — the same constant the
 * pacer has always used — and touches nothing else at all. Exposed so the offline
 * test can drive the whole path (including proving inertness) without the RT
 * thread or a socket. */
long reac_pacer_clock_tick(struct reac_pacer *p, uint64_t now_ns);

/* ---- HEALTH, published where an operator can see it (workstream CLK) --------
 *
 * THE FAULT THIS EXISTS FOR IS INVISIBLE BY CONSTRUCTION. The depth guard drops
 * 256 frames — 64 ms of audio — in one step so that PipeWire never starves, which
 * means no xrun is raised, no telemetry counter moves, and the console reports a
 * healthy graph while audio disappears. A signal that does not observe what it
 * claims to observe is exactly the defect family this project keeps paying for,
 * so the daemon states its own health in numbers rather than leaving it to be
 * inferred from a heartbeat nobody reads.
 *
 * WINDOWED, never cumulative: a lifetime counter cannot tell an operator whether
 * the desk is dropping audio RIGHT NOW. Everything here is a rate over the window
 * that just closed, except the two raw counters an operator may want to difference
 * by hand. Computed on the main loop from the pacer's atomics — the RT path is
 * untouched. */
/* Health window length. Long enough that the drift figure is not dominated by
 * one scheduler hiccup (at 900 ppm and 4000 fps the deficit is 3.6 frames/s, so a
 * 10 s window resolves it to ~3%), short enough that an operator sees a fault
 * appear rather than an average of the last hour. */
#define REAC_PACER_HEALTH_WINDOW_NS  10000000000ull

struct reac_pacer_health {
	int      valid;              /* 0 until the first full window has closed */
	double   window_s;           /* length of the window these rates cover */
	double   drift_ppm;          /* transmit deficit: (nominal - emitted) / nominal.
	                              * POSITIVE means we put fewer frames on the wire
	                              * than the rate asks for, so the TX ring grows. */
	double   discard_fps;        /* frames the depth guard discarded per second */
	double   discard_ms_per_s;   /* the same loss stated as audio: ms lost per second */
	double   late_wakes_ps;      /* late wakes per second */
	double   slots_dropped_ps;   /* unrepayable slot debt per second */
	double   slots_catchup_ps;   /* slot debt repaid on the grid per second */
	uint64_t tx_errors;          /* cumulative sendto() failures */
	uint64_t late_wakes;         /* cumulative */
	uint32_t ring_frames;        /* TX ring depth at the close of the window */
	uint32_t slot_debt_max;      /* largest SINGLE overslept debt in the window,
	                              * in slots. <= the catch-up budget means every
	                              * miss was repayable; above it is the tail. */
	double   ring_ms;            /* the same depth as graph->wire latency */
};

/* Fold one main-loop poll into the health window and, when a window closes,
 * write the rates into *out and return 1. Returns 0 (and leaves *out alone)
 * between windows. `now_ns` must be CLOCK_MONOTONIC (reac_pacer_mono_ns). */
int reac_pacer_health_poll(struct reac_pacer *p, uint64_t now_ns,
                           struct reac_pacer_health *out);

void reac_pacer_stop(struct reac_pacer *p);
void reac_pacer_close(struct reac_pacer *p);

#endif /* REAC_PACER_H */
