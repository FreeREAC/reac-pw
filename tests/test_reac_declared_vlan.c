// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* What SPLITS as a VLAN segment name, and — the half that matters — what does not.
 *
 * The rule is text, so it is pinned as text: the two forms that look like a VLAN and are
 * not (a dot with a tail that is not a number, an empty parent), the VID bounds (0 is the
 * priority tag, 4095 is reserved), the IFNAMSIZ refusal — because a TRUNCATED interface
 * name is a DIFFERENT interface and minting one would be worse than minting none — and the
 * table's no-duplicates and reported bound.
 *
 * WHO DECLARES is tests/test_reac_segconf.c's question, not this file's, since 2026-09-16.
 *
 * No netlink, no netdev, no filesystem. */
#include "reac_declared_vlan.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { \
	if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } \
} while (0)

static int splits(const char *seg, const char *want_parent, unsigned want_vid)
{
	char p[IFNAMSIZ] = { 0 };
	uint16_t v = 0;
	if (!reac_declared_vlan_split(seg, p, sizeof p, &v))
		return 0;
	return strcmp(p, want_parent) == 0 && v == want_vid;
}


static int rejects_split(const char *seg)
{
	char p[IFNAMSIZ];
	uint16_t v;
	return !reac_declared_vlan_split(seg, p, sizeof p, &v);
}

int main(void)
{
	/* 1. THE SEGMENT NAMES ON THE RIG. */
	CHECK(splits("enp131s0.11", "enp131s0", 11), "enp131s0.11 splits");
	CHECK(splits("enp131s0.4094", "enp131s0", 4094), "the top legal VID splits");
	CHECK(splits("eth0.1", "eth0", 1), "the bottom legal VID splits");
	/* QinQ: the split is at the LAST dot, so a VLAN over a VLAN names its real parent. */
	CHECK(splits("eth0.11.12", "eth0.11", 12), "a stacked VLAN names its own parent");

	/* 2. WHAT IS NOT A VLAN SEGMENT. Every one of these is a real string this code
	 *    meets on the rig, and each would mint a netdev nobody asked for. */
	CHECK(rejects_split("enp131s0"), "a bare interface is not a VLAN segment");
	CHECK(rejects_split("reac-pw"), "the conf file's own stem is not a VLAN segment");
	CHECK(rejects_split("enp131s0.env"), "a non-numeric tail is not a VID");
	CHECK(rejects_split("enp131s0."), "a trailing dot is not a VID");
	CHECK(rejects_split(".11"), "a VID with no parent is refused");
	CHECK(rejects_split("enp131s0.0"), "VID 0 is the priority tag, not a segment");
	CHECK(rejects_split("enp131s0.4095"), "VID 4095 is reserved");
	CHECK(rejects_split("enp131s0.65536"), "an out-of-range VID is refused");
	/* A TRUNCATED NAME IS A DIFFERENT INTERFACE. 15 chars fit, 16 do not. */
	CHECK(splits("abcdefghijk.111", "abcdefghijk", 111), "a 15-char netdev name fits");
	CHECK(rejects_split("abcdefghijkl.111"), "a 16-char netdev name is refused, not trimmed");

	/* 5. THE TABLE: no duplicates, and the bound is reported rather than hit silently. */
	struct reac_declared_vlan tab[3];
	int n = 0;
	CHECK(reac_declared_vlan_add(tab, 3, &n, "eth0", 11) == 1, "first add");
	CHECK(reac_declared_vlan_add(tab, 3, &n, "eth0", 11) == 0, "a duplicate is not added");
	CHECK(n == 1, "and the count did not move, got %d", n);
	CHECK(reac_declared_vlan_add(tab, 3, &n, "eth0", 12) == 1, "a different VID adds");
	CHECK(reac_declared_vlan_add(tab, 3, &n, "eth1", 11) == 1, "a different parent adds");
	CHECK(reac_declared_vlan_add(tab, 3, &n, "eth2", 11) == -1, "past the bound is refused");
	CHECK(n == 3, "and nothing was overwritten, got %d", n);

	/* THE KEY AND FILENAME FORMS ARE GONE, and their tests with them: a conf key's NAME
	 * and a `<seg>.env` file declared a segment as a side effect of a role projection,
	 * which outlived what declared it (the header's own reason, 2026-09-16). What
	 * declares a VLAN now is a `[segment <parent>.<vid>]` section, and
	 * tests/test_reac_segconf.c pins that.
	 */

	printf(fails ? "FAIL %d\n" : "OK\n", fails);
	return fails ? 1 : 0;
}
