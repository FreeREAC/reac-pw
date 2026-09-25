// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_source_node's rate renegotiation (task: "make the reac-CAPTURE node
 * follow a runtime rate change, mirroring the sink",
 * 2026-08-26-clock-tabs-and-reac-pace-coupling.md §1b: "a rate is ONE wire
 * rate — capture AND playback follow it together"). reac_source_node.c's
 * source_reconnect_rate and reac_source_node_publish_rate are NOT pure (they
 * own a live pw_stream, exactly like reac_sink_node.c's sink_reconnect_rate)
 * and cannot be exercised here — that needs a running graph and is the
 * parent's live pw-dump verification. What IS pure, and is exactly what those
 * two functions call, is reac_sink_format.h's shared builder + decisions
 * (reac_sink_format.h's revised SCOPE note explains why reac-capture now
 * shares them rather than owning a second hand-copied pair). This file proves
 * the source's OWN call shape against them:
 *
 *   1. reac_sink_format_build(&b, n->channels, hz) — called by source_
 *      reconnect_rate with the source node's channel count — produces a pod
 *      carrying the NEW rate, at REAC box widths (8/16/32/40; S-0808/S-1608/
 *      S-4000S/full fabric), not just the sink's arbitrary counts.
 *   2. reac_sink_format_needs_update(n->sample_rate, hz) — reac_source_node_
 *      publish_rate's own reconnect gate — fires exactly on an accepted
 *      change and stays false on a byte-identical poll and on a refused
 *      write (which never moves the pacer's rate_hz, the master's `hz` this
 *      module receives), proving a refusal reconnects nothing.
 *   3. reac_sink_format_rate_after_attempt — what n->sample_rate becomes
 *      after source_reconnect_rate's own pw_stream_connect() outcome: the
 *      requested rate on success, the previous rate on failure.
 *
 * Sabotage-verified (needs_update): an inverted comparison would make a real
 * rate change look like a no-op — proven by hand, not merely asserted, so a
 * regression that reintroduces this exact defect turns a currently-green
 * line red.
 *
 * What this does NOT and CANNOT prove: that PipeWire actually renegotiates
 * reac-capture's ACTIVE Format live, or that reac-capture ends up presenting
 * the SAME rate as reac-playback on the real wire — that is the parent's live
 * pw-dump verification of BOTH nodes on a clean rig (see the task's Honesty
 * section); this suite proves the pod and the decision are right, not the
 * live graph. */
#include "reac_sink_format.h"

#include <reac/reac.h>   /* REAC_MAX_CHANNELS */

#include <spa/param/audio/raw.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include <stdio.h>
#include <string.h>
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* source_reconnect_rate calls reac_sink_format_build(&fb, n->channels, hz)
 * exactly like this, at the box's real input width — not the sink's output
 * width, which is a different number on an asymmetric box (S-1608: 16 in /
 * 8 out). Prove the pod round-trips the NEW rate at each real box width. */
static int test_build_at_capture_widths_after_a_rate_change(void)
{
	const int widths[] = { REAC_BOX_S0808_IN, REAC_BOX_S1608_IN, REAC_BOX_S4000S_3208_IN,
	                       REAC_MAX_CHANNELS };
	for (size_t i = 0; i < sizeof widths / sizeof widths[0]; i++) {
		uint8_t buf[4096];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
		const struct spa_pod *pod = reac_sink_format_build(&b, widths[i], REAC_SAMPLE_RATE_44K1);
		CHK(pod != NULL);

		struct spa_audio_info_raw info;
		memset(&info, 0, sizeof info);
		CHK(spa_format_audio_raw_parse(pod, &info) >= 0);
		CHK(info.format == SPA_AUDIO_FORMAT_F32P);
		CHK(info.rate == REAC_SAMPLE_RATE_44K1);
		CHK(info.channels == (uint32_t)widths[i]);
		for (int c = 0; c < widths[i]; c++)
			CHK(info.position[c] == (uint32_t)(SPA_AUDIO_CHANNEL_AUX0 + c));
	}
	return 0;
}

/* reac_source_node_publish_rate(n, hz)'s own gate. A normal single-rate boot
 * (n->sample_rate seeded from the same rate the master pacer opened with)
 * must never look like a pending renegotiation. */
static int test_no_reconnect_on_matching_rate(void)
{
	CHK(reac_sink_format_needs_update(REAC_SAMPLE_RATE_48K, REAC_SAMPLE_RATE_48K) == 0);
	CHK(reac_sink_format_needs_update(REAC_SAMPLE_RATE_96K, REAC_SAMPLE_RATE_96K) == 0);
	return 0;
}

/* The exact case the operator measured live: playback followed a
 * `reac.cfg.rate` change, capture did not. This is the fix: the peer push
 * (reac_sink_node.c's sink_publish_rate_props -> reac_source_node_publish_
 * rate) hands the source the SAME accepted `hz`, and this gate must say so. */
static int test_reconnect_fires_on_accepted_change(void)
{
	CHK(reac_sink_format_needs_update(REAC_SAMPLE_RATE_48K, REAC_SAMPLE_RATE_44K1) == 1);
	CHK(reac_sink_format_needs_update(REAC_SAMPLE_RATE_44K1, REAC_SAMPLE_RATE_96K) == 1);
	return 0;
}

/* A REFUSED `reac.cfg.rate` write never moves the pacer's rate_hz — the exact
 * `hz` reac_sink_node.c's sink_publish_rate_props passes through to the peer
 * source unmodified. Modelled here as the pacer's rate staying exactly what
 * the source already presents: the gate must stay false, proving a refusal
 * reconnects reac-capture no more than it reconnects reac-playback. */
static int test_no_reconnect_after_a_refusal(void)
{
	int node_rate = REAC_SAMPLE_RATE_48K;
	int pacer_rate_after_refusal = REAC_SAMPLE_RATE_48K;   /* reac_rate_cfg_decide refused; unchanged */
	CHK(reac_sink_format_needs_update(node_rate, pacer_rate_after_refusal) == 0);
	return 0;
}

/* SABOTAGE: an inverted gate (`==` instead of `!=`) would make the exact bug
 * this task fixes look fixed while doing nothing — a real accepted change
 * would be read as "no update owed" and reac-capture would stay silently
 * stuck at boot rate again, the precise regression shape. Prove the real
 * function disagrees with that inversion on both arms, by hand. */
static int test_sabotage_inverted_gate_would_fail(void)
{
	int inverted_matching = (REAC_SAMPLE_RATE_48K == REAC_SAMPLE_RATE_48K);      /* what an inverted fn would answer */
	int inverted_changed  = (REAC_SAMPLE_RATE_48K == REAC_SAMPLE_RATE_44K1);
	CHK(inverted_matching == 1);   /* the WRONG answer for the matching case (want 0) */
	CHK(inverted_changed  == 0);   /* the WRONG answer for the changed case (want 1) */
	CHK(reac_sink_format_needs_update(REAC_SAMPLE_RATE_48K, REAC_SAMPLE_RATE_48K) != inverted_matching);
	CHK(reac_sink_format_needs_update(REAC_SAMPLE_RATE_48K, REAC_SAMPLE_RATE_44K1) != inverted_changed);
	return 0;
}

/* source_reconnect_rate's own bookkeeping: n->sample_rate must report only
 * what pw_stream_connect() actually proved, exactly like the sink's — the
 * requested rate on success, never on failure (which would be "applied:true
 * for work not done" applied to the capture node's own rate). */
static int test_rate_after_attempt_matches_connect_outcome(void)
{
	CHK(reac_sink_format_rate_after_attempt(REAC_SAMPLE_RATE_44K1, REAC_SAMPLE_RATE_48K, 1) == REAC_SAMPLE_RATE_44K1);
	CHK(reac_sink_format_rate_after_attempt(REAC_SAMPLE_RATE_44K1, REAC_SAMPLE_RATE_48K, 0) == REAC_SAMPLE_RATE_48K);
	return 0;
}

int main(void)
{
	int failed = 0;
	failed |= test_build_at_capture_widths_after_a_rate_change();
	failed |= test_no_reconnect_on_matching_rate();
	failed |= test_reconnect_fires_on_accepted_change();
	failed |= test_no_reconnect_after_a_refusal();
	failed |= test_sabotage_inverted_gate_would_fail();
	failed |= test_rate_after_attempt_matches_connect_outcome();
	if (failed) {
		fprintf(stderr, "test_reac_source_format: FAILED\n");
		return 1;
	}
	fprintf(stderr, "test_reac_source_format: all tests passed\n");
	return 0;
}
