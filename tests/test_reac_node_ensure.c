// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_node_ensure — the shared "same box, or rebuild?" decision behind
 * reac_sink_node_ensure and reac_source_node_ensure (see reac_node_ensure.h
 * for the bug this closes: reac_source_node_ensure used to compare width
 * only, so a same-width relabel left reac-capture's identity stale). Pure:
 * no pw_stream, no socket, no pacer.
 *
 * Proves:
 *   1. No existing node -> always a rebuild (nothing to compare against).
 *   2. Same width + same label -> no rebuild (the steady-state no-op both
 *      ensure() callers depend on to avoid churning a live node every poll).
 *   3. A width change -> rebuild, whatever the label.
 *   4. A LABEL-ONLY change (same width) -> rebuild — the exact case that was
 *      missing from reac_source_node_ensure before this fix.
 *   5. NULL and "" labels compare as identical (no false rebuild from a
 *      caller that passes NULL where another passes "").
 *
 * What this does NOT and CANNOT prove: that the caller's own destroy-before-
 * create sequencing (pw_stream_disconnect + pw_stream_destroy, then a fresh
 * pw_stream_new_simple + connect) leaves the PipeWire graph showing exactly
 * one node at any instant — that needs a running graph and is the live
 * pw-dump count docs/design/notes/2026-08-26-duplicate-reac-node.md asks
 * for. This proves only the decision that gates whether a rebuild happens at
 * all: given the same struct-level bookkeeping both node types already use
 * (one pointer, nulled before the replacement is built), the two callers now
 * agree on what "the same box" means. */
#include "reac_node_ensure.h"

#include <stdio.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static int test_no_existing_node_always_rebuilds(void)
{
	CHK(reac_node_ensure_needs_rebuild(false, 0, NULL, 8, "S-0808") == true);
	CHK(reac_node_ensure_needs_rebuild(false, 0, "", 0, "") == true);
	return 0;
}

static int test_identical_box_is_a_no_op(void)
{
	CHK(reac_node_ensure_needs_rebuild(true, 16, "S-1608", 16, "S-1608") == false);
	CHK(reac_node_ensure_needs_rebuild(true, 8, "", 8, "") == false);
	return 0;
}

static int test_width_change_rebuilds(void)
{
	CHK(reac_node_ensure_needs_rebuild(true, 8, "S-0808", 16, "S-0808") == true);
	CHK(reac_node_ensure_needs_rebuild(true, 16, "S-1608", 8, "S-1608") == true);
	return 0;
}

/* THE BUG THIS FIXES: same channel count, different label — a re-enrolled box
 * of the same width, or a relabelled pin. reac_source_node_ensure used to
 * miss this entirely (width-only compare); reac_sink_node_ensure already
 * caught it. Both now route through here. */
static int test_label_only_change_rebuilds(void)
{
	CHK(reac_node_ensure_needs_rebuild(true, 8, "Stage Left", 8, "S-0808") == true);
	CHK(reac_node_ensure_needs_rebuild(true, 16, "S-1608", 16, "S-4000S-alias") == true);
	return 0;
}

/* NULL and "" are the same fact (both callers seed want_label via
 * `label ? label : ""`), so a NULL cur_label must never look like a
 * different box from an explicit "". */
static int test_null_and_empty_label_are_identical(void)
{
	CHK(reac_node_ensure_needs_rebuild(true, 8, NULL, 8, "") == false);
	CHK(reac_node_ensure_needs_rebuild(true, 8, "", 8, NULL) == false);
	CHK(reac_node_ensure_needs_rebuild(true, 8, NULL, 8, NULL) == false);
	return 0;
}

/* SABOTAGE: dropping the label comparison (the exact regression this module
 * fixes — reac_source_node_ensure's old `cur->channels == want` check) would
 * make test_label_only_change_rebuilds's cases read as "no rebuild owed".
 * Prove the real function disagrees with that stripped-down comparison. */
static int test_sabotage_width_only_comparison_would_fail(void)
{
	int width_only_same_width_a = (8 == 8);     /* what a width-only check answers */
	int width_only_same_width_b = (16 == 16);
	CHK(width_only_same_width_a == 1);   /* the WRONG answer (want: rebuild -> true) */
	CHK(width_only_same_width_b == 1);
	CHK(reac_node_ensure_needs_rebuild(true, 8, "Stage Left", 8, "S-0808")
	    != !width_only_same_width_a);
	CHK(reac_node_ensure_needs_rebuild(true, 16, "S-1608", 16, "S-4000S-alias")
	    != !width_only_same_width_b);
	return 0;
}

int main(void)
{
	int failed = 0;
	failed |= test_no_existing_node_always_rebuilds();
	failed |= test_identical_box_is_a_no_op();
	failed |= test_width_change_rebuilds();
	failed |= test_label_only_change_rebuilds();
	failed |= test_null_and_empty_label_are_identical();
	failed |= test_sabotage_width_only_comparison_would_fail();
	if (failed) {
		fprintf(stderr, "test_reac_node_ensure: FAILED\n");
		return 1;
	}
	fprintf(stderr, "test_reac_node_ensure: all tests passed\n");
	return 0;
}
