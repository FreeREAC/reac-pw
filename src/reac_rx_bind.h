/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/* WHICH PEER THE RECEIVE GATE IS LOCKED TO, on a segment where we are not the master.
 *
 * reac_rx decodes one stream. Downstream (we are a slave or a box) it accepts every
 * 1492 B broadcast, and only once reac_rx_peer_reset names a peer does it compare the
 * source MAC. A master binds the gate to its box (reac_sink_node.c, from the pacer's
 * session); the slave side never did, so a second host broadcasting REAC frames on the
 * same wire was decoded into the same ring as the desk: its counters interleaved with the
 * desk's and its audio reached the graph. Same class as libreac's peer binding (the box
 * that is bound decides, one frame decides no role).
 *
 * TWO SOURCES FOR THE PEER, in this order:
 *
 *   hint     the MAC the hunt named when it decided this segment's verdict. libreac's hunt
 *            takes a rival into account only once it has streamed (REAC_HUNT_RIVAL_*), so
 *            one forged frame has already failed to become this. Applied at once, at
 *            open, before the feeder thread starts.
 *
 *   learned  the master MAC the slave engine took from the grant burst. That is one
 *            frame's word, so it is not trusted as one: it binds only when the gate has
 *            decoded REAC_RX_BIND_FRAMES frames and the SAME MAC has been the engine's
 *            answer for REAC_RX_BIND_STABLE_NS. A different MAC held for that long moves
 *            the lock (a desk replaced behind the same port must not leave the segment
 *            silent: rig 2026-08-21, the box that left and the gate that stayed); a
 *            different MAC seen for less moves nothing.
 *
 * Pure bookkeeping around reac_rx_peer_reset, called from the main loop only. The same
 * function is what main.c and the tests call, so the test drives the wiring, not a model
 * of it. */
#ifndef REAC_RX_BIND_H
#define REAC_RX_BIND_H

#include <stdint.h>

struct reac_rx;

/* The streaming bar libreac's hunt uses (REAC_HUNT_RIVAL_FRAMES / _SPAN_NS), restated
 * because the floor reac-pw builds against may predate them. tests/test_reac_rx_bind.c
 * asserts they agree wherever the library defines its own. */
#define REAC_RX_BIND_FRAMES     8u
#define REAC_RX_BIND_STABLE_NS  (500ULL * 1000000ULL)
/* The session number handed to reac_rx_peer_reset. The same MAC with the same session is
 * a no-op there, so a constant keeps the per-tick call free; a real session change
 * reaches the gate through reac_rx_session_end as before. */
#define REAC_RX_BIND_SESSION    1u

struct reac_rx_bind {
	uint8_t locked_mac[6];
	int locked;
	int from_hint;          /* the lock came from the hunt: only a hint replaces it */
	uint8_t cand[6];        /* the learned MAC being timed */
	int have_cand;
	uint64_t cand_since_ns;
};

void reac_rx_bind_init(struct reac_rx_bind *b);

/* The hunt's MAC for this segment. Locks the gate now. NULL mac is ignored. */
void reac_rx_bind_hint(struct reac_rx_bind *b, struct reac_rx *rx, const uint8_t mac[6]);

/* One main-loop tick. `learned_mac48` is the slave engine's master address packed
 * (reac_mac48_pack), 0 for none; `frames_ok` the gate's decoded-frame count.
 * Returns 1 when this call moved the lock. */
int reac_rx_bind_tick(struct reac_rx_bind *b, struct reac_rx *rx,
                      uint64_t learned_mac48, uint64_t frames_ok, uint64_t now_ns);

#endif
