// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_sink_node — the REAC-master TX sink node (reac:playback).
 *
 * FUNCTIONAL:
 *   - pw_filter registered Audio/Sink with N INPUT mono ports (the channel count)
 *   - realtime process() de-stages each quantum into 12-sample REAC frames,
 *     encodes them with reac_downstream_build (libreac), and SUBMITS them to the SCHED_FIFO cadence
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

struct reac_rx;   /* reac_rx.h — the BOX clock reference measurement source (#75) */

struct pw_loop;
struct reac_sink_node;
struct reac_source_node;       /* reac_source_node.h — the peer reac-capture node (#208) */
struct reac_box_model;         /* reac_ctrl.h — the autodetected box (in/out widths) */
struct reac_headamp_setting;   /* reac_headamp_tx.h — optional master head-amp table */

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
	/* Optional MASTER head-amp send table (task #155), forwarded to the pacer. */
	const struct reac_headamp_setting *headamps;
	int n_headamps;
	/* Clock discipline (#75), forwarded to the pacer. 0 (the default) = the pacer
	 * free-runs on CLOCK_MONOTONIC exactly as before and no reference is even
	 * read. See docs/ENV-KNOBS.md (REACPW_CLOCK_FOLLOW). */
	int clock_follow;
	/* Operator-DESIGNATED clock reference (#77): a case-insensitive SUBSTRING of
	 * the device name ("Babyface"), from REACPW_CLOCK_REF. A device that matches
	 * outranks every name heuristic — the operator knows their hardware and we do
	 * not. NULL/empty (the default) designates nothing, and nothing about the
	 * grading or the selection changes. Inert unless clock_follow is set. */
	const char *clock_ref;
};

/* Create the sink node = the REAC MASTER ENGINE: opens the AF_PACKET 0x8819 TX
 * socket on cfg->ifname (needs CAP_NET_RAW), starts the SCHED_FIFO cadence pacer
 * (which drives establishment AND recognizes the box on the wire) and the main-
 * loop event/log drain. The pw_filter GRAPH NODE is NOT created here — it is
 * DEFERRED to reac_sink_node_ensure so nothing is exposed in the graph until a
 * box is autodetected (or the caller sizes it explicitly). Returns NULL (and
 * logs) if the TX socket can't open. `tx_ring` is reserved for the future
 * slot-pacer cut; the direct-emit path ignores it. cfg->channels is ignored (the
 * width comes from ensure); cfg->label seeds the first ensure's label. */
struct reac_sink_node *reac_sink_node_new(struct pw_loop *loop,
                                          struct reac_ring *tx_ring,
                                          const struct reac_sink_cfg *cfg);

/* Bring the reac-playback GRAPH NODE to `channels` INPUT ports labelled `label`,
 * WITHOUT disturbing the running pacer/master (the sink owns the recognizer, so it
 * must never be torn down to resize). ONE entry point, callable from the
 * recognition path:
 *   - no filter yet                    -> create it at `channels`/`label`.
 *   - exists, same width AND label      -> no-op (identical box).
 *   - exists, width OR label changed    -> destroy + rebuild the pw_filter (the
 *       pacer keeps running throughout). A rebuild is used even when only the label
 *       changes (a same-out-width swap, e.g. S-1608 -> S-4000S) because
 *       pw_filter_update_properties does NOT re-stamp a live node's node.description
 *       / box-model / discovery props to the registry — only a fresh filter's
 *       creation-time props propagate.
 * Gain state + the pacer persist across a rebuild; the fresh filter's badge props
 * (link-state / box-model / discovery / latency) are stamped from the pacer
 * snapshot. Returns 0, or -1 on a failed (re)build. */
int reac_sink_node_ensure(struct reac_sink_node *n, int channels, const char *label);

/* The box model the pacer last recognized on the wire (its config-announce matched
 * a fixed-matrix row), or NULL if none yet. Cross-thread-safe (an atomic load of
 * the pacer's recognized_box) — the main-loop autodetect watcher polls this to
 * decide the reac-capture / reac-playback widths. */
const struct reac_box_model *reac_sink_node_recognized_box(const struct reac_sink_node *n);

/* Wire the peer reac-capture node's SLOT (#208) so the sink's main-loop badge timer
 * also keeps the source node's reac.link-state / box-model / box-width in sync — the
 * capture node has no pacer handle of its own. Pass the address of main's source-node
 * pointer (`&src`) so a source rebuilt on a live box-width change is followed. Call once
 * after both nodes exist; pass NULL slot to detach. */
void reac_sink_node_set_peer_source(struct reac_sink_node *n,
                                    struct reac_source_node **src_slot);

/* Wire the RX feeder as the BOX clock reference's measurement source (#75): the
 * sink's existing main-loop timer forwards reac_rx's filtered counter-slope ppm to
 * the pacer's discipline. Borrowed pointer, main-loop use only, no RT path
 * touched. Never wired -> the box tier is simply never available, and with clock
 * following disabled the forward is not even attempted. */
void reac_sink_node_set_rate_source(struct reac_sink_node *n, struct reac_rx *rx);

void reac_sink_node_destroy(struct reac_sink_node *n);

#endif /* REAC_SINK_NODE_H */
