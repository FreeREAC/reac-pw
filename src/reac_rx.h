// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_rx — the non-realtime RX feeder.
 *
 * Owns the wire source (live AF_PACKET via reac_capture, or pcap replay via
 * pcap_source), validates + decodes each 0x8819 frame with libreac's braid
 * core, converts the planar s24 output to float, and pushes it into the shared
 * ring for the PipeWire process() callback.
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
/* REAC_FRAME_BYTES_OHRCA + reac_frame_clean_len(): the OHRCA +2 length rule
 * moved to its one home in libreac (>= 0.3.0) — it applies to both directions,
 * not just this RX gate. What the 2 bytes are is still open (#80); the rule is
 * about length normalization and holds either way. */
#include <reac/reac.h>

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
	/* How many estimates have been PUBLISHED. ppm_error_milli is 0 both before the
	 * first window closes and when the slope is genuinely zero, so a consumer that
	 * steers anything off it needs to tell those apart — 0 with no estimate yet is
	 * "no information", not "the reference agrees with us". It also gives freshness
	 * for free: a counter that stops advancing is a reference that stopped
	 * producing (the clock-discipline BOX source, #75). */
	_Atomic uint32_t ppm_seq;

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
	/* Bumped when the PEER changes (reac_rx_peer_reset). A box plug is not a
	 * special case: it is a lost connection and a reconnect, so everything the
	 * loop learned from the previous peer — its counter continuity, its rate
	 * slope, the duplicate guard's previous frame — is stale the moment the MAC
	 * changes. The loop compares this against its own copy and clears in one
	 * place, so the reset cannot be half-applied. */
	unsigned peer_session;       /* the master session this lock belongs to */
	_Atomic unsigned peer_epoch;

	/* Duplicate-frame guard. Two different sources put the same frame on the
	 * wire twice, and both land here:
	 *
	 *   1. the OVER-CLOCK repeat — a 48 kHz box driven at the 96 kHz doubled
	 *      cadence (the #156 OHRCA path) re-transmits each frame verbatim,
	 *      same length, ~125 us apart (measured on the S-4000S: 100% of
	 *      adjacent same-counter pairs equal);
	 *   2. the MIRROR TWIN — a capture rig mirroring BOTH RX and TX of one port
	 *      sees a transiting frame twice, same src MAC and same counter, with
	 *      one copy carrying 2 bytes of the frame's own Ethernet FCS after the
	 *      C2 EA end marker and the other not. Those two copies differ in
	 *      LENGTH (1492 vs 1494 downstream, 628 vs 630 upstream, ...), which is
	 *      why the guard compares reac_frame_clean_len() bytes: on the clean
	 *      prefix the pair is byte-identical, on the wire length it never is.
	 *      Measured 2026-07-29 over the capture corpus: 61 of 83 captures carry
	 *      the twin, and on a mirrored S-1608 cold-connect the master's stream
	 *      is exactly 2 frames per counter (166,666 -> 83,333) while the box's
	 *      is 1 (83,334 -> 83,334, untouched).
	 *
	 * Feeding both copies concatenates every 12-sample block, so each block
	 * plays twice -> a granular per-frame stutter (the "granulated audio"
	 * symptom) and the effective rate doubles (96 kHz into a 48 kHz
	 * reac-capture -> overrun). Dropping the copy restores the true cadence.
	 *
	 * It cannot eat a genuine frame: the 16-bit counter is inside the compared
	 * bytes, so consecutive distinct frames are never byte-identical, and the
	 * corpus sweep found 0 adjacent same-counter pairs that differ in any byte
	 * (i.e. a same-counter pair is always a copy, never new audio). */
	uint8_t prev_frame[1560];   /* >= the rx frame buffer (REAC_FRAME_BYTES + 64) */
	size_t  prev_clean_len;     /* reac_frame_clean_len() of what prev_frame holds */
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
/* Point the UPSTREAM stream gate at a KNOWN box, replacing whatever it latched.
 * The gate locks onto the first box-shaped source it sees so two boxes cannot
 * interleave into one ring — but nothing ever unlocked it, so a hot swap left it
 * decoding a MAC that had left the segment: the new box's frames all failed the
 * compare, frames_ok froze, and reac-capture published silence while the wire
 * carried a live microphone. The master already knows which box is here; this is
 * how it says so, and a CHANGE resets the per-peer state with it. Safe to call
 * every tick with the same MAC: the same peer is a no-op. */
void reac_rx_peer_reset(struct reac_rx *rx, const uint8_t mac[6], unsigned session);

int reac_rx_start(struct reac_rx *rx);

/* Signal stop and join. */
void reac_rx_stop(struct reac_rx *rx);

void reac_rx_close(struct reac_rx *rx);

#endif /* REAC_RX_H */
