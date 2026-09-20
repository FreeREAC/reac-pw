// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_knobs.h"
#include "reac_code.h"

#include <reac/reac_envflag.h>
#include <reac/transport/reac_conf.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

const struct reac_knob g_reac_knobs[] = {
	/* per-segment, already layered at their read site (reac_conf_lookup) */
	{ "REAC_TX", 1, NULL },
	{ "REAC_MIXER", 1, NULL },
	{ "REAC_NAME", 1, NULL },
	{ "REAC_HEADAMP", 1, NULL },
	{ "REAC_SRC_MAC", 1, NULL },
	{ "REAC_BOX_CHANNELS", 1, NULL },
	{ "REAC_RATE", 1, NULL },
	{ "REAC_DEBUG", 1, NULL },
	{ "REACPW_BOX_MASTER_FRAME", 1, NULL },
	{ "REACPW_BOX_MASTER_BURST", 1, NULL },
	{ "REACPW_BOX_MASTER_FILL", 1, NULL },
	{ "REACPW_BOX_MASTER_PRESILENCE_MS", 1, NULL },
	{ "REACPW_CLOCK_FOLLOW", 1, NULL },
	/* REACPW_CLOCK_REF: was the one env-only exception (forwarded into a long-lived
	 * node that held the raw getenv() pointer, which a conf-layer buffer could not
	 * safely stand in for). reac_source_node.c now copies it into its own fixed
	 * buffer (matching reac_sink_node.c's existing n->clock_ref[64] pattern), so
	 * there is no longer a lifetime reason to keep this env-only — it is
	 * conf_capable like every #75/#77 sibling. */
	{ "REACPW_CLOCK_REF", 1, NULL },
	{ "REACPW_CATCHUP_MAX_SLOTS", 1, NULL },
	{ "REACPW_RATE_MATCH", 1, NULL },
	/* libreac-transport's own, already layered there (reac_pacer.c, reac_rt.c) */
	{ "REACPW_PACER", 1, NULL },
	{ "REACPW_PACER_LEAD_US", 1, NULL },
	{ "REACPW_RT_PRIO", 1, NULL },
	/* libreac-internal (reac_master.c/reac_pacer.c/reac_ifscan.c/reac_rx.c). Read
	 * through reac_*_tunables_set() (libreac's docs/design/specs/
	 * 2026-09-17-tunables-api-and-shared-refusal-codes.md) — this daemon resolves
	 * the value itself (reac_knobs_resolve, same table, same precedence as every
	 * other knob here) and pushes it in before the transport starts
	 * (push_libreac_tunables, main.c). conf_capable like every other row now that
	 * libreac 1.3.0 ships the tunables API this lane was blocked on. */
	{ "REACPW_GRANT_ON_DECLARE", 1, NULL },
	{ "REACPW_GRANT_DWELL_MS", 1, NULL },
	{ "REACPW_GRANT_DWELL_S", 1, NULL },
	{ "REACPW_NO_ENROLL", 1, NULL },
	{ "REACPW_EST_SCENE", 1, NULL },
	{ "REAC_IFACES_ALLOW_WIRELESS", 1, NULL },
	{ "REACPW_GUARD_FLOOR_FRAMES", 1, NULL },
	{ "REACPW_NO_HEADAMP", 1, NULL },
	/* WHAT THE PHYSICAL PORT ACTUALLY CARRIES, when sysfs cannot say or when the
	 * operator knows better (#107, auto-role amendment 2026-09-20). The link-budget
	 * admission reads /sys/class/net/<port>/speed; a veth reports 10 Gbit and a netns
	 * has no such file at all, and a NIC that negotiates 1 Gbit into a 100 Mbit uplink
	 * reports the negotiation and not the uplink. Per-port through the segment layer
	 * (REACPW_LINK_MBIT_<port>, the PHYSICAL port's name — a VLAN's budget is its
	 * parent's), host-wide otherwise. */
	{ "REACPW_LINK_MBIT", 1, NULL },
};

const int g_reac_knobs_count = (int)(sizeof g_reac_knobs / sizeof g_reac_knobs[0]);

int reac_conf_flag(const char *key, int dflt)
{
	char v[16];
	if (reac_conf_lookup(key, NULL, NULL, v, sizeof v) == REAC_CONF_NONE)
		return dflt;
	int parsed = reac_envflag_parse(v);
	return parsed < 0 ? dflt : parsed;
}

/* ---- the command line, highest precedence (operator ruling, 2026-09-17) --------- */

#define REAC_KNOBS_ARGV_MAX 32

static struct {
	const char *key;      /* points into argv[] — process-lifetime storage, no copy needed */
	char value[256];
} g_argv[REAC_KNOBS_ARGV_MAX];
static int g_argv_n;

static int knob_known(const char *key)
{
	for (int i = 0; i < g_reac_knobs_count; i++)
		if (strcmp(g_reac_knobs[i].key, key) == 0)
			return 1;
	return 0;
}

int reac_knobs_set_argv(const char *key, const char *value)
{
	if (!key || !*key || !knob_known(key))
		return 0;
	for (int i = 0; i < g_argv_n; i++) {
		if (strcmp(g_argv[i].key, key) == 0) {
			snprintf(g_argv[i].value, sizeof g_argv[i].value, "%s", value ? value : "");
			return 1;
		}
	}
	if (g_argv_n >= REAC_KNOBS_ARGV_MAX)
		return 0;   /* --set given more times than any real command line has knobs */
	g_argv[g_argv_n].key = key;
	snprintf(g_argv[g_argv_n].value, sizeof g_argv[g_argv_n].value, "%s", value ? value : "");
	g_argv_n++;
	return 1;
}

static const char *argv_lookup(const char *key)
{
	for (int i = 0; i < g_argv_n; i++)
		if (strcmp(g_argv[i].key, key) == 0)
			return g_argv[i].value;
	return NULL;
}

enum reac_conf_layer reac_knobs_resolve(const char *key, char *out, size_t cap)
{
	const char *v = argv_lookup(key);
	if (v) {
		snprintf(out, cap, "%s", v);
		return REAC_CONF_ARGV;
	}
	return reac_conf_lookup(key, NULL, NULL, out, cap);
}

enum reac_conf_layer reac_knobs_resolve_port(const char *key, const char *name,
                                             char *out, size_t cap)
{
	const char *v = argv_lookup(key);
	if (v) {
		snprintf(out, cap, "%s", v);
		return REAC_CONF_ARGV;
	}
	return reac_conf_lookup(key, name, NULL, out, cap);
}

int reac_knobs_resolve_flag(const char *key, int dflt)
{
	char v[256];
	if (reac_knobs_resolve(key, v, sizeof v) == REAC_CONF_NONE)
		return dflt;
	int parsed = reac_envflag_parse(v);
	return parsed < 0 ? dflt : parsed;
}

int reac_knobs_announce(FILE *out)
{
	int set = 0, unset = 0;
	for (int i = 0; i < g_reac_knobs_count; i++) {
		const struct reac_knob *k = &g_reac_knobs[i];
		char v[512];
		const char *layer_name;
		if (k->conf_capable) {
			enum reac_conf_layer l = reac_knobs_resolve(k->key, v, sizeof v);
			if (l == REAC_CONF_NONE) {
				unset++;
				continue;
			}
			layer_name = (l == REAC_CONF_ARGV) ? "cli"
			           : (l == REAC_CONF_HOST || l == REAC_CONF_LAST_RESORT) ? "conf"
			           : "env";
		} else {
			const char *e = getenv(k->key);
			if (!e || !*e) {
				unset++;
				continue;
			}
			snprintf(v, sizeof v, "%s", e);
			layer_name = "env";
		}
		reac_code_emit(out, "reac-pw", RC_S_KNOB_SET,
		                "knob %s=%s (%s)\n", k->key, v, layer_name);
		set++;
	}
	reac_code_emit(out, "reac-pw", RC_S_KNOB_SUMMARY,
	                "knobs: %d set, %d default\n", set, unset);
	return set;
}
