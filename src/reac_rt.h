// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_rt — WHERE reac-pw's own real-time threads sit in the host's SCHED_FIFO
 * ladder, and the one place that decides it.
 *
 * reac-pw runs two kinds of wire-clock thread outside the PipeWire graph: the
 * MASTER cadence pacer (reac_pacer.c), which must put a frame on the wire every
 * slot period, and the SLAVE upstream engine (reac_slave.c), which must answer
 * every received master frame within one slot. Both need SCHED_FIFO — a
 * SCHED_OTHER wire clock jitters far enough that a box will not link.
 *
 * THE LAW: A REAC WIRE CLOCK SITS BELOW EVERYTHING THE AUDIO GRAPH NEEDS.
 *
 * The ladder this host actually runs (`ps -eLo cls,rtprio,comm`, and
 * /usr/share/pipewire/pipewire.conf's `rt.prio`):
 *
 *   FF 99   kernel migration threads
 *   FF 60   the PipeWire SERVER data-loop — the graph DRIVER, the thread the
 *           audio interface's cycle runs on
 *   FF 55   every PipeWire CLIENT data-loop — the console's DSP, mod-host, and
 *           reac-pw's OWN graph node
 *   FF 50   threaded kernel IRQs, including the REAC NICs
 *   FF 45   >>> reac-pw's wire clocks <<<
 *
 * Three reasons the wire clock goes UNDER all of them, not over:
 *
 *   1. THE PACER CAN REPAY A LATE SLOT; THE AUDIO DRIVER CANNOT REPAY A LATE
 *      QUANTUM. The pacer holds an absolute deadline grid and settles slot debt
 *      out of a bounded budget (REACPW_CATCHUP_MAX_SLOTS). A driver that misses
 *      its quantum has no such instrument: it xruns, and the operator hears it.
 *      Where the two must contend, the one that can recover is the one that
 *      waits.
 *   2. BELOW THE CLIENT TIER, NOT MERELY BELOW THE DRIVER. A cycle is not
 *      finished when the driver has run; it is finished when every client has
 *      answered. A thread sitting between 55 and 60 preempts the console's DSP
 *      loop and produces exactly the same xrun one at 79 does — and that loop is
 *      the pacer's own PRODUCER, so starving it to feed the wire on time is
 *      self-defeating.
 *   3. BELOW THE THREADED IRQs. The pacer's per-slot work is a sendto() into the
 *      NIC's tx queue and a bounded recv() drain of the same NIC. Both need that
 *      NIC's IRQ thread to make progress. A wire clock scheduled above its own
 *      interrupt handler starves the packets it exists to move.
 *
 * 45 is one clear band below the IRQ tier: above every SCHED_OTHER task on the
 * box (any FIFO priority is), and with room left between it and 50 for an
 * operator to place a REAC-adjacent helper without disturbing this decision.
 *
 * A host whose PipeWire is configured to a different ladder is a real case, so
 * the number is CONFIGURABLE: REACPW_RT_PRIO, resolved through the layered
 * config (reac_conf.h) with no per-segment layer — the scheduler ladder is a
 * property of the HOST, not of one NIC. A configured value is honoured as
 * given, including one that outranks the graph: the operator may know something
 * about their host that this file does not. It is reported, and a value that
 * would preempt the audio graph is reported LOUDLY, because the failure it
 * causes (xruns on an interface reac-pw never touches) points nowhere near
 * here. */
#ifndef REAC_RT_H
#define REAC_RT_H

/* The measured tiers above us. Named so the reasoning above is checkable and
 * the test can pin the default against them. */
#define REAC_RT_PW_DRIVER_PRIO   60   /* pipewire.conf rt.prio: the graph driver */
#define REAC_RT_PW_CLIENT_PRIO   55   /* every client data-loop, ours included   */
#define REAC_RT_KERNEL_IRQ_PRIO  50   /* threaded IRQs, the REAC NICs among them */

/* Where a REAC wire clock runs. */
#define REAC_RT_PRIO_DEFAULT     45

/* The knob, and the file layer that can carry it. */
#define REAC_RT_PRIO_KEY "REACPW_RT_PRIO"

/* Which layer decided the priority in force. Reported to the operator, because
 * a scheduling number nobody can trace back to its source is a number every
 * reader assumes a different origin for. */
enum reac_rt_prio_source {
	REAC_RT_PRIO_SRC_BUILTIN = 0,  /* nothing configured: REAC_RT_PRIO_DEFAULT */
	REAC_RT_PRIO_SRC_CALLER,       /* an explicit non-zero prio from the caller */
	REAC_RT_PRIO_SRC_CONFIG,       /* REACPW_RT_PRIO answered                   */
	REAC_RT_PRIO_SRC_REFUSED,      /* REACPW_RT_PRIO was not a usable priority;
	                                * the built-in is in force and said so      */
};

/* A short human phrase for the source. Never NULL. */
const char *reac_rt_prio_source_name(enum reac_rt_prio_source s);

/* PURE. Parse one configured value. NULL, empty or all-blank is not an answer
 * (the reac_conf.h rule: a key someone blanked out is a key they turned off).
 * A usable priority is a whole number in 1..99 with no trailing junk; anything
 * else is REFUSED rather than coerced, because a priority silently clamped to
 * something the operator did not write is the kind of number that gets believed.
 * Always returns a runnable priority — the built-in default when it refuses. */
int reac_rt_prio_parse(const char *value, enum reac_rt_prio_source *src);

/* PURE. Does a thread at `prio` contend with the PipeWire audio graph? True at
 * the client tier as well as above it: an equal-priority runnable thread still
 * delays a graph loop waiting for the same CPU. */
int reac_rt_prio_preempts_audio(int prio);

/* Resolve the priority for a wire-clock thread, highest layer first:
 * an explicit `caller_prio` (> 0), then REACPW_RT_PRIO through reac_conf, then
 * the built-in. `home` is the base for ~ and exists so the test can point the
 * config stack at a temporary directory; pass NULL for the real $HOME.
 *
 * Reads files, so call it from the thread that SPAWNS the wire clock, never
 * from inside one. */
int reac_rt_prio_resolve(int caller_prio, const char *home,
                         enum reac_rt_prio_source *src);

/* Put the CALLING thread on SCHED_FIFO at `prio` and report the outcome on
 * stderr under `who`. Returns 0 on success, -1 when the privilege is missing —
 * the caller keeps running at SCHED_OTHER, which is jittery but functional, so
 * this is a report and not a fatal error. */
int reac_rt_thread_go(const char *who, int prio, enum reac_rt_prio_source src);

#endif /* REAC_RT_H */
