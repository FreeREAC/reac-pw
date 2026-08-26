// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_sink_format — the reac-playback node's Format pod, and the pure
 * decision to renegotiate it
 * (docs/design/specs/2026-08-26-clock-tabs-and-reac-pace-coupling.md §4.3
 * increment 3). Pure: no pw_stream, no socket, no pacer.
 *
 * Proves:
 *   1. reac_sink_format_build's pod round-trips (parsed with
 *      spa_format_audio_raw_parse, not merely re-read from the builder call's
 *      own argument) to the exact rate/channels/position asked for, at the
 *      boot rate and again after a change — the half the offline suite CAN
 *      prove: the pod that reaches pw_stream_update_params carries the
 *      asserted rate.
 *   2. channel count clamps into [0, REAC_MAX_CHANNELS] rather than walking
 *      the position array out of bounds.
 *   3. reac_sink_format_needs_update fires exactly when the node's
 *      last-pushed rate has fallen behind the pacer's standing rate: false on
 *      a byte-identical boot (node_rate == pacer_rate), true on an accepted
 *      change, and false again once caught up — and a REFUSED write (which
 *      never touches the pacer's rate_hz, so pacer_rate stays exactly what it
 *      was) leaves it false throughout, proving a refusal never renegotiates
 *      the format either.
 *
 * What this does NOT and CANNOT prove: that PipeWire actually renegotiates a
 * live node's Format when pw_stream_update_params is called with this pod —
 * that needs a running graph and is the operator's live verification
 * (pw-dump's Format.rate after a PATCH), exactly the gap this increment
 * closes and exactly what an offline test structurally cannot observe. */
#include "reac_sink_format.h"

#include <reac/reac.h>   /* REAC_MAX_CHANNELS */

#include <spa/param/audio/raw.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static int test_build_round_trips_the_rate(void)
{
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);

	const struct spa_pod *pod = reac_sink_format_build(&b, 8, 48000);
	CHK(pod != NULL);

	struct spa_audio_info_raw info;
	memset(&info, 0, sizeof info);
	CHK(spa_format_audio_raw_parse(pod, &info) >= 0);
	CHK(info.format == SPA_AUDIO_FORMAT_F32P);
	CHK(info.rate == 48000);
	CHK(info.channels == 8);
	for (int c = 0; c < 8; c++)
		CHK(info.position[c] == (uint32_t)(SPA_AUDIO_CHANNEL_AUX0 + c));
	return 0;
}

/* The exact case the gap note measured: an accepted `reac.cfg.rate` moves the
 * asserted rate from the boot value to a new one. The pod built for the new
 * rate must carry THAT rate, not the old one — proving the rebuild, not just
 * the initial connect, produces the right value. */
static int test_build_after_a_rate_change(void)
{
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);

	const struct spa_pod *pod = reac_sink_format_build(&b, 16, 96000);
	CHK(pod != NULL);

	struct spa_audio_info_raw info;
	memset(&info, 0, sizeof info);
	CHK(spa_format_audio_raw_parse(pod, &info) >= 0);
	CHK(info.rate == 96000);
	CHK(info.channels == 16);
	return 0;
}

static int test_channel_count_clamps(void)
{
	uint8_t buf[4096];

	/* A caller value past the fabric width clamps to REAC_MAX_CHANNELS rather
	 * than walking spa_audio_info_raw.position out of bounds. */
	struct spa_pod_builder b1 = SPA_POD_BUILDER_INIT(buf, sizeof buf);
	const struct spa_pod *pod = reac_sink_format_build(&b1, REAC_MAX_CHANNELS + 100, 48000);
	CHK(pod != NULL);
	struct spa_audio_info_raw info;
	memset(&info, 0, sizeof info);
	CHK(spa_format_audio_raw_parse(pod, &info) >= 0);
	CHK(info.channels == REAC_MAX_CHANNELS);

	/* A negative count clamps to zero, not to a huge unsigned wrap. */
	struct spa_pod_builder b2 = SPA_POD_BUILDER_INIT(buf, sizeof buf);
	pod = reac_sink_format_build(&b2, -3, 48000);
	CHK(pod != NULL);
	memset(&info, 0, sizeof info);
	CHK(spa_format_audio_raw_parse(pod, &info) >= 0);
	CHK(info.channels == 0);
	return 0;
}

/* Byte-exactness: a normal single-rate boot must never look like a pending
 * renegotiation. node_rate is seeded from the SAME rate the pacer opens with
 * (reac_sink_node_new sets n->sample_rate from cfg->sample_rate, which is
 * exactly what reac_pacer_open derives fps from), so the two always agree at
 * t=0. */
static int test_no_update_on_matching_rate(void)
{
	CHK(reac_sink_format_needs_update(48000, 48000) == 0);
	CHK(reac_sink_format_needs_update(96000, 96000) == 0);
	return 0;
}

/* An accepted `reac.cfg.rate` (reac_pacer_apply_rate) moves p->rate_hz — the
 * atomic sink_publish_rate_props reads as `hz` — away from the node's
 * last-pushed n->sample_rate. That mismatch is exactly what must fire the
 * renegotiation. */
static int test_update_fires_on_accepted_change(void)
{
	CHK(reac_sink_format_needs_update(48000, 96000) == 1);
	CHK(reac_sink_format_needs_update(96000, 44100) == 1);
	return 0;
}

/* A REFUSED `reac.cfg.rate` write (out-of-list, not drivable, malformed, or
 * asserted against a slave — reac_rate_cfg_decide) never calls
 * reac_pacer_request_rate, so the pacer's rate_hz never moves. Modelled here
 * as pacer_rate staying at the node's already-current rate: the decision must
 * stay false, proving a refusal renegotiates nothing. */
static int test_no_update_after_a_refusal(void)
{
	int node_rate = 48000;
	int pacer_rate_after_refusal = 48000;   /* reac_rate_cfg_decide refused; unchanged */
	CHK(reac_sink_format_needs_update(node_rate, pacer_rate_after_refusal) == 0);
	return 0;
}

/* A pacer_rate of 0 or negative cannot come from a real accepted rate
 * (reac_rate_cfg_decide's closed list starts at 44100), so treating it as "no
 * update owed" refuses to push a degenerate Format rather than propagating
 * garbage into the graph. */
static int test_no_update_on_non_positive_pacer_rate(void)
{
	CHK(reac_sink_format_needs_update(48000, 0) == 0);
	CHK(reac_sink_format_needs_update(48000, -1) == 0);
	return 0;
}

/* SABOTAGE: an inverted needs_update (`==` instead of `!=`) would make a real
 * rate change look like a no-op and an already-caught-up poll look like a
 * pending renegotiation — the two failure modes that would silently
 * reintroduce this exact bug (a self-reported rate believed over the node's
 * actual Format) or spam pw_stream_update_params on every 200 ms poll. Prove
 * the two tests above actually disagree with that inversion, by hand, rather
 * than trusting the comparison operator visually. */
static int test_sabotage_inverted_comparison_would_fail(void)
{
	int inverted_matching = (48000 == 48000);      /* what an inverted fn would answer */
	int inverted_changed  = (48000 == 96000);
	CHK(inverted_matching == 1);   /* the WRONG answer for the matching case (want 0) */
	CHK(inverted_changed  == 0);   /* the WRONG answer for the changed case (want 1) */
	CHK(reac_sink_format_needs_update(48000, 48000) != inverted_matching);
	CHK(reac_sink_format_needs_update(48000, 96000) != inverted_changed);
	return 0;
}

int main(void)
{
	int failed = 0;
	failed |= test_build_round_trips_the_rate();
	failed |= test_build_after_a_rate_change();
	failed |= test_channel_count_clamps();
	failed |= test_no_update_on_matching_rate();
	failed |= test_update_fires_on_accepted_change();
	failed |= test_no_update_after_a_refusal();
	failed |= test_no_update_on_non_positive_pacer_rate();
	failed |= test_sabotage_inverted_comparison_would_fail();
	if (failed) {
		fprintf(stderr, "test_reac_sink_format: FAILED\n");
		return 1;
	}
	fprintf(stderr, "test_reac_sink_format: all tests passed\n");
	return 0;
}
