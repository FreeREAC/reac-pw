// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_sink_node — the REAC-master TX sink node (reac:playback).
 *
 * FUNCTIONAL:
 *   - pw_filter registered Audio/Sink with N INPUT mono ports (the channel count)
 *   - realtime process() de-stages each quantum into 12-sample REAC frames,
 *     encodes them with reac_tx_build, and SUBMITS them to the SCHED_FIFO cadence
 *     pacer (reac_pacer) — NO syscall on the RT graph thread.
 *   - the pacer thread clocks the wire at a fixed pps and stamps the master
 *     JOIN/HOLD handshake (reac_master: probe -> cdea 04 03 grant -> established
 *     cdea 01 03 channel-map + cfea announce ~1/s) onto the downstream broadcast,
 *     so a real Roland stagebox slaves to us. PipeWire resamples the app rate
 *     INTO our advertised REAC rate.
 *
 * Verified by construction + loopback (a tone reaches the wire FILLER audio); a
 * real desk LINKING is the hardware-verify gate (no desk on the bench). See
 * DESIGN.md S2 (master handshake) + S6 (pacer) + the hardware-verify gate.
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
	uint8_t console_field;    /* emulated mixer model: 0 = V-Mixer (M-200/M-300),
	                           * 1 = OHRCA (M-5000). Drives cfea [19] + ENROLL.   */
	const char *inst;         /* per-instance node suffix -> "reac-playback.<inst>"
	                           * so one master per REAC VLAN coexists. NULL = bare. */
	const char *label;        /* operator box name for the node description        */
};

/* Create the sink node. Opens an AF_PACKET 0x8819 TX socket on cfg->ifname
 * (needs CAP_NET_RAW) and registers cfg->channels INPUT ports; process() encodes
 * + emits. Returns NULL (and logs) if the TX socket can't open. `tx_ring` is
 * reserved for the future slot-pacer cut; the direct-emit path ignores it. */
struct reac_sink_node *reac_sink_node_new(struct pw_loop *loop,
                                          struct reac_ring *tx_ring,
                                          const struct reac_sink_cfg *cfg);

void reac_sink_node_destroy(struct reac_sink_node *n);

#endif /* REAC_SINK_NODE_H */
