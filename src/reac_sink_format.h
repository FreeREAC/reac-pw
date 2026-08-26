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
 * builder is handed a caller-owned buffer, and the decision functions take
 * plain ints. Unit-testable offline, same shape as reac_rate_cfg.
 *
 * INCREMENT 4 (this file's second decision, reac_sink_format_rate_after_
 * attempt): increment 3's `pw_stream_update_params(EnumFormat)` call was
 * measured live to change nothing — it advertises a new supported set but
 * does not force an already-streaming adapter to renegotiate its ACTIVE
 * format. The mechanism fix (reac_sink_node.c's sink_reconnect_rate:
 * pw_stream_disconnect + pw_stream_connect at the new Format, a graph-side
 * re-establish) needs a live pw_stream and cannot be proven here; what IS
 * pure and provable is what n->sample_rate should become given the
 * attempt's outcome — see the function's own comment.
 *
 * SCOPE, REVISED (2026-08-26-clock-tabs-and-reac-pace-coupling.md §1b: "a rate
 * is ONE wire rate — capture AND playback follow it together"): this module's
 * pod-builder and both PURE decisions are node-agnostic (channels + rate in,
 * a pod or a verdict out) and are now shared by reac_source_node.c's own
 * renegotiate path (source_reconnect_rate), not reac-playback alone. That
 * sharing is deliberate, not incidental: the two nodes must renegotiate to
 * the exact same shape, and a second hand-copied builder is exactly the kind
 * of per-node split the spec forbids.
 *
 * The two nodes still differ in WHICH rate they follow. reac-playback (the
 * master TX sink) is authoritatively the pacer's rate_hz — the same atomic
 * sink_publish_rate_props already reads — so a MASTER's reac-capture also
 * follows rate_hz: it is the same wire, so it is the same fact, pushed to the
 * peer node via reac_source_node_publish_rate (see reac_sink_node.c's
 * sink_publish_rate_props, which calls it once it has decided the pacer's
 * rate moved). A SLAVE's reac-capture rate is `reac_rx`'s own recovered/
 * forced wire rate, a separate mechanism with no live update path of its own
 * today (set once at reac_rx_open, never re-detected) — wiring that live
 * would first need reac_rx itself to re-detect or be told the new rate,
 * which is a reac_rx change, not a format-pod change, and stays a TODO (see
 * reac_source_node.h's reac_source_node_publish_rate doc); a slave rate
 * change is not operator-driven the way a master's `reac.cfg.rate` write is,
 * so this is not on the path any current control exercises. */
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

/* PURE: what should the node's presented sample_rate become after a live
 * reconnect attempt at `requested_hz`? `connect_ok` is the caller's
 * pw_stream_connect() outcome for that request (nonzero = succeeded).
 *
 * On success the requested rate is what the node now genuinely presents, so
 * it is adopted. On failure the node's format was never proven to have
 * moved — `prev_hz` is what it is still known to present (the rate the
 * caller's own fallback reconnect re-asserts) — so THAT is returned, never
 * `requested_hz`. Getting this backwards would have the node claim a rate
 * pw_stream_connect() just refused: the "applied:true for work not done"
 * failure this project's discipline refuses everywhere else, applied here to
 * a node's own rate bookkeeping instead of a controller's answer. */
int reac_sink_format_rate_after_attempt(int requested_hz, int prev_hz, int connect_ok);

#endif /* REAC_SINK_FORMAT_H */
