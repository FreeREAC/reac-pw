// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* What counts as a DECLARED VLAN segment, and — the half that matters — what does not.
 *
 * The rule is text, so it is pinned as text: the conf keys and the per-segment filenames
 * an operator actually writes, the two forms that look like a VLAN and are not
 * (`reac-pw.env`, `enp131s0.env` — a dot and a tail that is not a number), the VID bounds
 * (0 is the priority tag, 4095 is reserved), and the IFNAMSIZ refusal, because a
 * TRUNCATED interface name is a different interface and minting one would be worse than
 * minting none.
 *
 * No netlink, no netdev, no filesystem beyond a scan of a directory this test makes. */
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

static int keys(const char *key, const char *want_parent, unsigned want_vid)
{
	char p[IFNAMSIZ] = { 0 };
	uint16_t v = 0;
	if (!reac_declared_vlan_from_key(key, p, sizeof p, &v))
		return 0;
	return strcmp(p, want_parent) == 0 && v == want_vid;
}

static int files(const char *fname, const char *want_parent, unsigned want_vid)
{
	char p[IFNAMSIZ] = { 0 };
	uint16_t v = 0;
	if (!reac_declared_vlan_from_filename(fname, p, sizeof p, &v))
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

	/* 3. THE KEY FORMS. The VALUE is never read: naming the segment IS the declaration,
	 *    so a segment declared only by its rate is declared. */
	CHECK(keys("REAC_ROLE_enp131s0.11", "enp131s0", 11), "REAC_ROLE_<seg> declares");
	CHECK(keys("REAC_RATE_enp131s0.12", "enp131s0", 12), "REAC_RATE_<seg> declares");
	CHECK(keys("REAC_BOX_CHANNELS_eth0.13", "eth0", 13), "a multi-word key declares");
	CHECK(!keys("REAC_ROLE", NULL, 0), "a bare key declares nothing");
	CHECK(!keys("REAC_ROLE_enp131s0", NULL, 0), "a non-VLAN segment declares no VLAN");
	CHECK(!keys("REACPW_PACER_LEAD_US", NULL, 0), "a knob is not a segment");
	CHECK(!keys("PATH_eth0.11", NULL, 0), "only REAC* keys declare");

	/* 4. THE FILENAME FORM, and the two files that sit beside it in the same directory. */
	CHECK(files("enp131s0.11.env", "enp131s0", 11), "<seg>.env declares");
	CHECK(files("/home/x/.config/reac-pw/eth0.12.env", "eth0", 12), "a full path declares");
	CHECK(!files("reac-pw.env", NULL, 0), "the daemon's own conf file declares nothing");
	CHECK(!files("enp131s0.env", NULL, 0), "a per-parent conf file declares no VLAN");
	CHECK(!files("enp131s0.11.env.hand-written", NULL, 0), "a backup is not a declaration");
	CHECK(!files("enp131s0.11", NULL, 0), "a name with no .env suffix is not a file form");

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

	/* 6. A WHOLE CONF FILE, comments, blanks and quoting included. */
	const char *text =
		"# generated by openmixer\n"
		"REAC_ROLE=slave\n"
		"REAC_ROLE_enp131s0.11=master\n"
		"  REAC_RATE_enp131s0.12 = 96000\n"
		"#REAC_ROLE_enp131s0.13=master\n"
		"REAC_ROLE_enp131s0.11=master\n"
		"\n"
		"REACPW_CLOCK_FOLLOW=1\n";
	struct reac_declared_vlan t2[8];
	int n2 = 0;
	int added = reac_declared_vlan_scan_text(text, t2, 8, &n2);
	CHECK(added == 2, "the file declares exactly two segments, got %d", added);
	CHECK(n2 == 2, "and the table holds two, got %d", n2);
	/* A COMMENTED-OUT DECLARATION IS NOT A DECLARATION. An operator who commented a
	 * segment out and still got its netdev minted would have no way to turn one off. */
	for (int i = 0; i < n2; i++)
		CHECK(t2[i].vid != 13, "a commented-out key declared vid 13");

	printf(fails ? "FAIL %d\n" : "OK\n", fails);
	return fails ? 1 : 0;
}
