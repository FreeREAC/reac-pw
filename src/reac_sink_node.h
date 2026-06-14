// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_sink_node — the virtual-stagebox TX sink node. INTERFACE ONLY.
 *
 * Scaffolded but NOT functional: the REAC TX layer it depends on (frame
 * builder, plain-LE interleaver (s*N+ch)*3, data[31] checksum-apply,
 * free-running u16-LE counter stamper) does NOT yet exist in libreac. libreac
 * today is RX/measure-only (grep-verified: no builder, no checksum-apply, no
 * interleave, no emitter). See NATIVE-REAC-DESIGN.md Section 2 + Section 6, and
 * roadmap steps 5-7.
 *
 * The shape mirrors the source node so that, once the TX layer lands, this
 * fills in symmetrically:
 *   - pw_filter registered Audio/Sink with N INPUT ports (the box's input count)
 *   - realtime process() copies one quantum/channel into a TX ring
 *   - a SCHED_FIFO slot pacer (the reac_repacer.c pattern: prio ~79, mlockall,
 *     affinity, clock_nanosleep TIMER_ABSTIME) wakes every slot period
 *     (125.0/250.0/272.1 us), pulls 12 samples/ch, runs the TX layer, emits on
 *     0x8819. Here the WIRE cadence is the rate authority, so PipeWire resamples
 *     the graph INTO our fixed pps (the mirror of the source's follower role).
 *
 * Phase 2 also needs the JOIN/HOLD connection FSM (flood FILLER on PHY-up ->
 * cold-connect cdea 04 03 -> config-announce -> heartbeat cdea 01 03 0001 81 at
 * ~1/s -> walk the 40-ch map, never deduped -> stable src-MAC). None of that is
 * in scope for this first cut; this header reserves the surface so the RX node
 * and the (future) TX node share one client + one pair of rings.
 */
#ifndef REAC_SINK_NODE_H
#define REAC_SINK_NODE_H

#include "reac_ring.h"

struct pw_loop;
struct reac_sink_node;

struct reac_sink_cfg {
	const char *ifname;   /* TX NIC (raw AF_PACKET 0x8819) */
	int channels;         /* the box's input count (<= 40) */
	int sample_rate;      /* fixed pps authority: 44100/48000/96000 */
	const uint8_t *src_mac;   /* stable Roland-OUI src MAC for the virtual box */
	const uint8_t *master_mac;/* unicast destination once linked */
};

/* Create the sink node skeleton. Returns NULL and logs "TX layer unbuilt" — the
 * node registers its input ports for graph visibility but produces NO wire
 * traffic until the libreac TX layer + slot pacer exist. The TX `ring` is the
 * symmetric counterpart of the RX ring. */
struct reac_sink_node *reac_sink_node_new(struct pw_loop *loop,
                                          struct reac_ring *tx_ring,
                                          const struct reac_sink_cfg *cfg);

void reac_sink_node_destroy(struct reac_sink_node *n);

/* ---- the TX layer this node BLOCKS ON (to be added to libreac) ---- *
 *
 * These are declared here as the contract the sink node will call once libreac
 * grows a TX layer; they are intentionally NOT implemented in reac-pw (TX is a
 * libreac concern, reused not reinvented — same way RX decode lives in the
 * reac-aes67 core). Listed so the dependency is explicit and greppable.
 *
 *   int  reac_tx_build_frame(uint8_t *frame, const float *planar,
 *                            int channels, int samples, uint16_t counter);
 *   void reac_tx_apply_checksum(uint8_t *data32);     // sum%256==0; FILLER exempt
 *   void reac_tx_stamp_counter(uint8_t *frame, uint16_t counter); // bytes 14-15 LE
 *
 * Until those exist, reac_sink_node_new returns a non-NULL skeleton that holds
 * ports but never emits. */

#endif /* REAC_SINK_NODE_H */
