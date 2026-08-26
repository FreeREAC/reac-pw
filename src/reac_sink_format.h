// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_sink_format — the reac-playback node's Format pod, and the decision to
 * renegotiate it, factored out of reac_sink_node.c so both are provable
 * without a live pw_stream
 * (docs/design/specs/2026-08-26-clock-tabs-and-reac-pace-coupling.md §4.3
 * increment 3, closing the gap docs/design/notes/2026-08-26-rate-change-node-
 * format-gap.md measured on the rig).
 *
 * THE BUG THIS FIXES: an accepted `reac.cfg.rate` already re-clocks the wire
 * (reac_pacer_apply_rate) and already re-publishes reac.rate/.state as node
 * PROPERTIES (reac_sink_node.c's sink_publish_rate_props) — but the pw_stream
 * node's own Format param, the thing pw-top and the graph actually read, was
 * built ONCE at connect and never touched again. A self-reported prop moved;
 * the node's presented rate did not. This module is the fix: ONE format-build
 * function shared by the initial connect and every later renegotiation, and a
 * PURE decision of when a renegotiation is owed.
 *
 * PURE: no PipeWire stream object, no socket, no RT privilege — the pod
 * builder is handed a caller-owned buffer, and the decision function takes
 * two plain ints. Unit-testable offline, same shape as reac_rate_cfg.
 *
 * SCOPE: reac-playback (the master TX sink) only. Its presented rate is
 * authoritatively the pacer's rate_hz — the same atomic sink_publish_rate_
 * props already reads — so the pacer's existing accepted-rate signal is the
 * ONE path this renegotiation rides. reac-capture (reac_source_node) is
 * DELIBERATELY NOT extended here: its Format rate is `reac_rx`'s own
 * recovered/forced wire rate, a separate mechanism with no live update path
 * of its own today (set once at reac_rx_open, never re-detected). Wiring it
 * to reac_pacer_apply_rate would invent a second, parallel rate channel
 * exactly where the discipline governing this increment forbids one; giving
 * reac-capture a live rate would first need reac_rx itself to re-detect or
 * be told the new rate, which is a reac_rx change, not a format-pod change,
 * and is out of this increment. */
#ifndef REAC_SINK_FORMAT_H
#define REAC_SINK_FORMAT_H

struct spa_pod_builder;
struct spa_pod;

/* Build the SPA_PARAM_EnumFormat pod for `channels` AUX-mapped F32 PLANAR
 * ports at `rate` Hz — the exact shape reac_sink_node's connect-time format
 * has always used (F32P keeps the stage's per-channel layout; AUX0..AUXn
 * marks each box output as a discrete mono send so no graph tool pairs two
 * as stereo). `channels` is clamped into [0, REAC_MAX_CHANNELS] so a bad
 * caller value cannot walk the position array out of bounds. Builds into
 * `b`; the caller owns the backing buffer and its lifetime (same convention
 * as spa_format_audio_raw_build itself). Returns the pod — never NULL, since
 * spa_format_audio_raw_build cannot fail on a big-enough builder buffer. */
const struct spa_pod *reac_sink_format_build(struct spa_pod_builder *b,
                                             int channels, int rate);

/* PURE decision: has the node's presented Format fallen behind the pacer's
 * standing rate? `node_rate` is the rate the node's Format was last built/
 * pushed at (reac_sink_node's own n->sample_rate shadow); `pacer_rate` is
 * reac_pacer.rate_hz's current value (reac.rate — the accepted rate, boot
 * value or the last `reac.cfg.rate` re-establish). Returns 1 when they
 * differ AND pacer_rate is a real positive rate (never 1 on a zero/negative
 * pacer_rate, which would otherwise push a degenerate Format instead of
 * refusing to act on garbage); 0 otherwise — including the boot case where
 * nothing has ever diverged, which is what keeps a normal single-rate boot
 * byte-identical (no renegotiation ever fires). */
int reac_sink_format_needs_update(int node_rate, int pacer_rate);

#endif /* REAC_SINK_FORMAT_H */
