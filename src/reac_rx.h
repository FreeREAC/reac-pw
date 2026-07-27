// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_rx — the non-realtime RX feeder.
 *
 * Owns the wire source (live AF_PACKET via reac_capture, or pcap replay via
 * pcap_source), validates + decodes each 0x8819 frame with the reac-aes67
 * plain-LE core, converts the planar s24 output to float, and pushes it into
 * the shared ring for the PipeWire process() callback.
 *
 * It is ALSO the rate authority: it tracks the byte-14/15 free-running counter
 * slope against CLOCK_MONOTONIC (or a NIC PHC), which gives both the recovered
 * REAC sample rate (via reac_rate_snap) and a slowly-filtered ppm error. The
 * source node reads that ppm and feeds it to PipeWire's io_rate_match so the
 * graph's async resampler tracks the desk's true rate (the Tier-A clock
 * bridge). This thread does NOT run SCHED_FIFO — it's the producer; only the
 * audio graph and (in Phase 2) the TX slot pacer are realtime.
 */
#ifndef REAC_RX_H
#define REAC_RX_H

#include <stdint.h>
#include <pthread.h>
#include "reac_ring.h"

/* OHRCA (M-5000/M-480) downstream frame length: the standard REAC_FRAME_BYTES
 * (1492) plus a 2-byte per-frame CRC-16 trailer appended after the C2 EA end
 * marker (measured on live M-5000 captures, 2026-07-11). Defined here (reac-pw
 * owned) rather than in libreac's reac.h, which stays pristine as an upstream
 * wrap subproject. Kept as a literal to avoid include-order coupling. */
#define REAC_FRAME_BYTES_OHRCA 1494

enum reac_rx_kind {
	REAC_RX_PCAP,    /* offline replay (pcap_source) */
	REAC_RX_LIVE,    /* live AF_PACKET (reac_capture) */
};

/* Which of the two REAC audio streams this RX decodes. The wire carries BOTH
 * directions (the master's 40-ch broadcast + each box's box-width return);
 * feeding a mixed stream into one ring would interleave two different audio
 * sources and corrupt the counter/ppm tracking, so the role picks exactly one:
 *   slave  -> DOWNSTREAM: the master's 1492 B / 40-ch broadcast (we lock to it)
 *   master -> UPSTREAM:   a box's 52+nch*36 B braided return (its inputs)      */
enum reac_rx_accept {
	REAC_RX_ACCEPT_DOWNSTREAM = 0, /* default: the historical 40-ch path */
	REAC_RX_ACCEPT_UPSTREAM,
};

struct reac_rx_cfg {
	enum reac_rx_kind kind;
	const char *source;   /* ifname for LIVE, file path for PCAP */
	int forced_rate;      /* 0 = auto-detect from cadence; else 44100/48000/96000 */
	int pcap_realtime;    /* PCAP only: pace replay by capture timestamps (1) vs flat out (0) */
	enum reac_rx_accept accept; /* which stream to decode (zero-init = DOWNSTREAM) */
};

struct reac_rx {
	struct reac_rx_cfg cfg;
	struct reac_ring *ring;       /* shared with the source node */
	int sample_rate;              /* recovered (snapped) REAC rate */

	pthread_t thread;
	_Atomic int running;

	/* rate-slope estimator (counter-vs-monotonic), filtered ppm error vs the
	 * nominal recovered rate; the source node reads this for io_rate_match. */
	_Atomic int ppm_error_milli;  /* ppm * 1000, signed; 0 until enough samples */

	/* update_ppm() window state — instance-owned (was function-static, which
	 * survived a pcap-loop restart and made RX a non-reentrant singleton). Reset
	 * alongside the pcap-restart reset in rx_loop. */
	uint64_t ppm_win_start_ns;
	uint32_t ppm_win_frames;
	uint16_t ppm_last_counter;
	int      ppm_have_last;

	/* UPSTREAM accept: lock onto the first box's src MAC so a second box's
	 * return can't interleave into the same ring/counter stream. Multi-box RX
	 * (the 40-slot allocation) is a separate lane. */
	uint8_t up_src[6];
	int     up_src_locked;

	/* OHRCA duplicate-frame guard. A 48 kHz box driven at the 96 kHz doubled
	 * cadence (the #156 OHRCA path) re-transmits each frame BYTE-IDENTICALLY —
	 * measured on the S-4000S: 100% of adjacent same-counter pairs are equal,
	 * ~125 us apart (a real second transmission, not a capture mirror). Feeding
	 * both copies concatenates every 12-sample block, so each block plays twice
	 * -> a granular per-frame stutter (the "granulated audio" symptom) and the
	 * effective rate doubles (96 kHz into a 48 kHz reac-capture -> overrun).
	 * We drop the exact repeat: it carries no new audio, and genuine distinct
	 * frames (32 ch x 12 samp x 24-bit) are never byte-identical, so a true
	 * 48 kHz box (no duplication) and a real 96 kHz box (distinct frames) are
	 * both unaffected. Dropping the copy restores the true rate into the ring. */
	uint8_t prev_frame[1560];   /* >= the rx frame buffer (REAC_FRAME_BYTES + 64) */
	size_t  prev_frame_len;
	int     have_prev_frame;

	/* diagnostics */
	_Atomic uint64_t frames_ok;
	_Atomic uint64_t frames_bad;
	_Atomic uint64_t frames_other; /* valid REAC, but the OTHER stream (gated out) */
	_Atomic uint64_t counter_gaps; /* lost frames inferred from counter jumps */
	_Atomic uint64_t frames_dup;   /* OHRCA byte-identical duplicates dropped */

	/* the source node (RT thread) publishes its last ring-read stats here so the
	 * non-RT telemetry below can print them — keeps fprintf off the RT path. */
	_Atomic int src_peak_micro;    /* peak sample * 1e6 across linked ports (fine) */
	_Atomic int src_active_ch;     /* linked ports carrying signal this read */
	_Atomic int src_fill;          /* ring backlog after the read (samples) */
};

/* Open the source, detect (or accept the forced) rate, and allocate the ring
 * sized for the rate. Does NOT start the thread. Returns 0 / -1.
 * On success rx->sample_rate and rx->ring are valid. */
int reac_rx_open(struct reac_rx *rx, const struct reac_rx_cfg *cfg, struct reac_ring *ring);

/* Spawn the feeder thread. Returns 0 / -1. */
int reac_rx_start(struct reac_rx *rx);

/* Signal stop and join. */
void reac_rx_stop(struct reac_rx *rx);

void reac_rx_close(struct reac_rx *rx);

#endif /* REAC_RX_H */
