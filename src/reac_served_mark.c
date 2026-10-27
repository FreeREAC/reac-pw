// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The served-interface mark. See reac_served_mark.h for why it is an alias. */
#include "reac_served_mark.h"
#include "reac_link_alias.h"
#include "reac_code.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Where this module's lines go: NULL is standard error, which is no constant. */
static FILE *g_say;

void reac_served_say_to(FILE *out)
{
	g_say = out;
}

static FILE *say(void)
{
	return g_say ? g_say : stderr;
}

enum reac_served_verdict reac_served_decide(const char *kind, const char *alias)
{
	if (alias && alias[0]) {
		if (strncmp(alias, REAC_ALIAS_PREFIX, sizeof REAC_ALIAS_PREFIX - 1) == 0)
			return REAC_SERVED_OURS;
		return REAC_SERVED_LEAVE_HOST;
	}
	if (kind && strcmp(kind, "vlan") == 0)
		return REAC_SERVED_LEAVE_VLAN;
	return REAC_SERVED_MARK;
}

static void remember(struct reac_served_marks *m, const char *ifname)
{
	for (int i = 0; i < m->n; i++)
		if (strcmp(m->name[i], ifname) == 0)
			return;
	if (m->n >= REAC_SERVED_MAX)
		return;
	snprintf(m->name[m->n++], sizeof m->name[0], "%s", ifname);
}

int reac_served_mark(struct reac_served_marks *m, const char *ifname)
{
	if (!m || !ifname || !ifname[0])
		return -EINVAL;
	char kind[32], alias[256];
	int rc = reac_link_alias_read(ifname, kind, sizeof kind, alias, sizeof alias);
	if (rc != 0) {
		reac_code_emit(say(), "reac-pw", RC_E_NOT_MARKED,
		        "[%s] could not read the interface alias (errno %d) — not marked as a "
		        "REAC interface, so the host's network manager is not told to leave it "
		        "alone\n", ifname, -rc);
		return rc;
	}
	enum reac_served_verdict v = reac_served_decide(kind, alias);
	switch (v) {
	case REAC_SERVED_MARK:
		rc = reac_link_alias_write(ifname, REAC_SERVED_ALIAS);
		if (rc != 0) {
			reac_code_emit(say(), "reac-pw", RC_E_NOT_MARKED,
			        "[%s] could not mark it '%s' (errno %d)%s — the "
			        "host's network manager is not told to leave it alone\n",
			        ifname, REAC_SERVED_ALIAS, -rc,
			        rc == -EPERM ? ": this daemon has no CAP_NET_ADMIN" : "");
			return rc;
		}
		remember(m, ifname);
		break;
	case REAC_SERVED_OURS:
		/* A mark an earlier run left behind (it did not exit cleanly) is ours to
		 * take away at exit; a minted VLAN's is libreac's and goes with the netdev. */
		if (strcmp(alias, REAC_SERVED_ALIAS) == 0)
			remember(m, ifname);
		break;
	case REAC_SERVED_LEAVE_VLAN:
		fprintf(say(), "reac-pw: [%s] is a VLAN the host made — not marked; if a "
		        "network manager configures it, give it no addresses there\n", ifname);
		break;
	case REAC_SERVED_LEAVE_HOST:
		fprintf(say(), "reac-pw: [%s] carries the host's own alias '%s' — not marked; "
		        "the host's network manager is not told to leave it alone\n",
		        ifname, alias);
		break;
	}
	return (int)v;
}

void reac_served_unmark_all(struct reac_served_marks *m)
{
	if (!m)
		return;
	for (int i = 0; i < m->n; i++) {
		char alias[256];
		if (reac_link_alias_read(m->name[i], NULL, 0, alias, sizeof alias) != 0)
			continue;
		if (strcmp(alias, REAC_SERVED_ALIAS) != 0)
			continue;
		reac_link_alias_write(m->name[i], "");
	}
	memset(m, 0, sizeof *m);
}
