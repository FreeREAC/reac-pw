// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_served_mark's policy over a SCRIPTED kernel.
 *
 * The two rtnetlink doors (reac_link_alias_read/_write) are replaced at link time
 * (-Wl,--wrap in meson.build), so the policy under test is the real reac_served_mark.c and
 * no device is touched. The claims:
 *
 *   1. a physical NIC with no alias is marked, and a clean exit removes the mark;
 *   2. an alias the host set is never overwritten, and never removed at exit;
 *   3. a VLAN the host made (no alias) is left alone;
 *   4. a minted VLAN (`reac-pw:minted`) is not re-marked and not unmarked by us;
 *   5. a mark an earlier run left behind is adopted and removed at exit;
 *   6. a mark somebody replaced after we wrote it is not removed at exit;
 *   7. a refused write (no CAP_NET_ADMIN) is reported and nothing is remembered. */
#include "reac_served_mark.h"
#include "reac_link_alias.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fake_if {
	const char *name;
	const char *kind;
	char        alias[64];
	int         writes;
};

static struct fake_if ifs[8];
static int n_ifs;
static int write_rc;

static struct fake_if *find(const char *name)
{
	for (int i = 0; i < n_ifs; i++)
		if (strcmp(ifs[i].name, name) == 0)
			return &ifs[i];
	return NULL;
}

/* For the assertions: an interface the test itself added, never NULL. */
static struct fake_if *get(const char *name)
{
	struct fake_if *f = find(name);
	if (!f) {
		printf("FAIL: the test asked for '%s', which it never added\n", name);
		exit(1);
	}
	return f;
}

static void add(const char *name, const char *kind, const char *alias)
{
	struct fake_if *f = &ifs[n_ifs++];
	memset(f, 0, sizeof *f);
	f->name = name;
	f->kind = kind;
	snprintf(f->alias, sizeof f->alias, "%s", alias);
}

int __wrap_reac_link_alias_read(const char *name, char *kind, size_t kind_n,
                                char *alias, size_t alias_n);
int __wrap_reac_link_alias_read(const char *name, char *kind, size_t kind_n,
                                char *alias, size_t alias_n)
{
	struct fake_if *f = find(name);
	if (!f)
		return -ENODEV;
	if (kind)
		snprintf(kind, kind_n, "%s", f->kind);
	if (alias)
		snprintf(alias, alias_n, "%s", f->alias);
	return 0;
}

int __wrap_reac_link_alias_write(const char *name, const char *alias);
int __wrap_reac_link_alias_write(const char *name, const char *alias)
{
	struct fake_if *f = find(name);
	if (!f)
		return -ENODEV;
	if (write_rc)
		return write_rc;
	f->writes++;
	snprintf(f->alias, sizeof f->alias, "%s", alias);
	return 0;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
	/* The pure verdict, one row per case. */
	CHECK(reac_served_decide("", "") == REAC_SERVED_MARK, "physical, no alias");
	CHECK(reac_served_decide("veth", "") == REAC_SERVED_MARK, "veth, no alias");
	CHECK(reac_served_decide("vlan", "") == REAC_SERVED_LEAVE_VLAN, "host vlan");
	CHECK(reac_served_decide("vlan", "reac-pw:minted") == REAC_SERVED_OURS, "minted vlan");
	CHECK(reac_served_decide("", "reac-pw:served") == REAC_SERVED_OURS, "left-behind mark");
	CHECK(reac_served_decide("", "uplink") == REAC_SERVED_LEAVE_HOST, "host alias");
	CHECK(reac_served_decide(NULL, NULL) == REAC_SERVED_MARK, "NULLs read as empty");

	add("enp1s0", "", "");                    /* 1 */
	add("lab0", "", "lab-port");              /* 2 */
	add("enp1s0.11", "vlan", "");             /* 3 */
	add("enp1s0.12", "vlan", "reac-pw:minted"); /* 4 */
	add("stale0", "", "reac-pw:served");      /* 5 */
	add("moved0", "", "");                    /* 6 */

	struct reac_served_marks m = { 0 };
	CHECK(reac_served_mark(&m, "enp1s0") == REAC_SERVED_MARK, "enp1s0 verdict");
	CHECK(strcmp(get("enp1s0")->alias, REAC_SERVED_ALIAS) == 0, "enp1s0 marked");
	CHECK(reac_served_mark(&m, "enp1s0") == REAC_SERVED_OURS, "re-serve is ours");
	CHECK(get("enp1s0")->writes == 1, "a re-serve writes nothing");
	CHECK(reac_served_mark(&m, "lab0") == REAC_SERVED_LEAVE_HOST, "lab0 verdict");
	CHECK(reac_served_mark(&m, "enp1s0.11") == REAC_SERVED_LEAVE_VLAN, "host vlan verdict");
	CHECK(reac_served_mark(&m, "enp1s0.12") == REAC_SERVED_OURS, "minted verdict");
	CHECK(reac_served_mark(&m, "stale0") == REAC_SERVED_OURS, "stale verdict");
	CHECK(reac_served_mark(&m, "moved0") == REAC_SERVED_MARK, "moved0 verdict");
	CHECK(reac_served_mark(&m, "gone0") == -ENODEV, "no such interface is -ENODEV");
	CHECK(m.n == 3, "remembered enp1s0, stale0, moved0 (got %d)", m.n);

	snprintf(get("moved0")->alias, sizeof get("moved0")->alias, "%s", "renamed-by-host");
	reac_served_unmark_all(&m);
	CHECK(get("enp1s0")->alias[0] == '\0', "enp1s0 unmarked at exit");
	CHECK(get("stale0")->alias[0] == '\0', "left-behind mark removed at exit");
	CHECK(strcmp(get("moved0")->alias, "renamed-by-host") == 0, "a replaced mark is kept");
	CHECK(strcmp(get("lab0")->alias, "lab-port") == 0, "host alias untouched");
	CHECK(get("lab0")->writes == 0, "host alias never written");
	CHECK(get("enp1s0.11")->writes == 0 && get("enp1s0.11")->alias[0] == '\0',
	      "host vlan untouched");
	CHECK(strcmp(get("enp1s0.12")->alias, "reac-pw:minted") == 0 &&
	      get("enp1s0.12")->writes == 0, "minted alias untouched");
	CHECK(m.n == 0, "marks cleared");

	/* 7: a refused write. */
	add("nocap0", "", "");
	write_rc = -EPERM;
	CHECK(reac_served_mark(&m, "nocap0") == -EPERM, "refused write is -EPERM");
	CHECK(m.n == 0, "a refused mark is not remembered");
	CHECK(get("nocap0")->alias[0] == '\0', "nothing written");

	if (fails)
		return 1;
	printf("OK: reac_served_mark marks served NICs, leaves the host's own, unmarks on exit\n");
	return 0;
}
