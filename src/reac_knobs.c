// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_knobs.h"
#include "reac_code.h"

#include <reac/reac_envflag.h>
#include <reac/transport/reac_conf.h>

#include <stdlib.h>
#include <string.h>

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
	{ "REACPW_CLOCK_REF", 0,
	  "forwarded verbatim into a long-lived node that does not copy it; a conf-layer "
	  "buffer would dangle once this call returns" },
	{ "REACPW_CATCHUP_MAX_SLOTS", 1, NULL },
	{ "REACPW_RATE_MATCH", 1, NULL },
	/* libreac-transport's own, already layered there (reac_pacer.c, reac_rt.c) */
	{ "REACPW_PACER", 1, NULL },
	{ "REACPW_PACER_LEAD_US", 1, NULL },
	{ "REACPW_RT_PRIO", 1, NULL },
	/* libreac-internal, read with a bare getenv in reac_master.c/reac_pacer.c. This
	 * lane links the SYSTEM libreac-devel package (pkg-config libreac, 1.2.2), not the
	 * sibling checkout, and may not cut a libreac release (tools/reac-release is the
	 * main session's) -- so these cannot be made conf_capable from here without
	 * shipping a header/lookup libreac does not have yet. Owed: spec §6. */
	{ "REACPW_GRANT_ON_DECLARE", 0,
	  "read in libreac's reac_master.c; this lane links the system libreac-devel "
	  "package and cannot cut a libreac release to add the layering there" },
	{ "REACPW_GRANT_DWELL_MS", 0,
	  "read in libreac's reac_master.c; same reason as REACPW_GRANT_ON_DECLARE" },
	{ "REACPW_GRANT_DWELL_S", 0,
	  "read in libreac's reac_master.c; same reason as REACPW_GRANT_ON_DECLARE" },
	{ "REACPW_NO_ENROLL", 0,
	  "read in libreac's reac_master.c; same reason as REACPW_GRANT_ON_DECLARE" },
	{ "REACPW_EST_SCENE", 0,
	  "read in libreac's reac_master.c; same reason as REACPW_GRANT_ON_DECLARE" },
	{ "REAC_IFACES_ALLOW_WIRELESS", 0,
	  "read in libreac-transport's reac_ifscan.c; same reason as REACPW_GRANT_ON_DECLARE" },
	{ "REACPW_GUARD_FLOOR_FRAMES", 0,
	  "read in libreac-transport's reac_pacer.c; same reason as REACPW_GRANT_ON_DECLARE" },
	{ "REACPW_NO_HEADAMP", 0,
	  "read in libreac-transport's reac_pacer.c; same reason as REACPW_GRANT_ON_DECLARE" },
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

int reac_knobs_announce(FILE *out)
{
	int set = 0, unset = 0;
	for (int i = 0; i < g_reac_knobs_count; i++) {
		const struct reac_knob *k = &g_reac_knobs[i];
		char v[512];
		const char *layer_name;
		if (k->conf_capable) {
			enum reac_conf_layer l = reac_conf_lookup(k->key, NULL, NULL, v, sizeof v);
			if (l == REAC_CONF_NONE) {
				unset++;
				continue;
			}
			layer_name = (l == REAC_CONF_HOST || l == REAC_CONF_LAST_RESORT)
			                 ? "conf" : "env";
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
