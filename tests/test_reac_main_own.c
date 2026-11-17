// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* main.c's per-segment decisions -- what one segment's conf keys do to its listener --
 * with no PipeWire server, no interface and no wire.
 *
 * This #includes src/main.c (main() renamed away) so the statics are reachable, loads a
 * real reac-pw.conf from a temporary HOME, and reads the result off the structs main()
 * would start the engine from. The netns scripts proved the same things through a running
 * daemon, one slow boot each; here the question "does the file's key reach the listener"
 * is a function call.
 *
 * Pinned: the role / tap / ignore keys of a segment, where the role came from, the box
 * row's width and the key that may not move it, the node-name defaults, the layered keys
 * (TX, MIXER, NAME, HEADAMP, SRC_MAC, BOX_CHANNELS) with the refusal of a malformed one,
 * the rate resolution order, and the deferral to a foreign desk. */
#define main reac_pw_daemon_main_not_run
#include "../src/main.c"
#undef main

#include <ftw.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static char g_home[64];

static int rm_cb(const char *p, const struct stat *sb, int flag, struct FTW *ftw)
{
	(void)sb; (void)flag; (void)ftw;
	return remove(p);
}

static void home_remove(void)
{
	if (g_home[0])
		nftw(g_home, rm_cb, 8, FTW_DEPTH | FTW_PHYS);
	g_home[0] = '\0';
}

/* A fresh HOME holding `conf` as the one override file, loaded into g_segconf. */
static int home_with_conf(const char *conf)
{
	home_remove();
	snprintf(g_home, sizeof g_home, "reac-main-own-XXXXXX");
	if (!mkdtemp(g_home))
		return -1;
	char path[160];
	snprintf(path, sizeof path, "%s/.config", g_home);
	mkdir(path, 0700);
	snprintf(path, sizeof path, "%s/.config/reac-pw", g_home);
	mkdir(path, 0700);
	snprintf(path, sizeof path, "%s/.config/reac-pw/reac-pw.conf", g_home);
	FILE *f = fopen(path, "w");
	if (!f)
		return -1;
	fputs(conf, f);
	fclose(f);
	char abs[PATH_MAX];
	if (!realpath(g_home, abs))
		return -1;
	setenv("HOME", abs, 1);
	memset(&g_segconf, 0, sizeof g_segconf);
	reac_segconf_load(&g_segconf, abs);
	return 0;
}

static void clear_env(void)
{
	static const char *keys[] = {
		"REAC_TX", "REAC_MIXER", "REAC_NAME", "REAC_HEADAMP", "REAC_SRC_MAC", "REAC_BOX_CHANNELS",
		"REAC_RATE", "REAC_TX_eth9", "REAC_MIXER_eth9", "REAC_NAME_eth9", "REAC_HEADAMP_eth9",
		"REAC_SRC_MAC_eth9", "REAC_BOX_CHANNELS_eth9", "REAC_RATE_eth9", NULL };
	for (int i = 0; keys[i]; i++)
		unsetenv(keys[i]);
}

/* ---- the role keys of one segment ------------------------------------------------ */
static int test_role_keys(void)
{
	CHK(home_with_conf(
		"[segment eth1]\nrole = master\n"
		"[segment eth2]\nrole = slave\n"
		"[segment eth3]\nrole = tap\n"
		"[segment eth4]\nrole = auto\n"
		"[segment eth5]\nignore = yes\n") == 0);
	clear_env();
	struct listener_cfg c;
	enum reac_role r = REAC_ROLE_SLAVE;

	/* a pinned master: a SETTING, served on the first frame, from the file */
	listener_cfg_from_conf(&c, "eth1", 1);
	CHK(c.role_pinned && c.role == REAC_ROLE_MASTER && c.role_intent == REAC_ROLE_INTENT_MASTER);
	CHK(c.role_layer == REAC_CONF_SEGMENT && !c.tap);
	CHK(segment_role_pin("eth1", &r) == 1 && r == REAC_ROLE_MASTER);
	CHK(!strcmp(segment_role_source("eth1"), REAC_SEGCONF_FILE));
	CHK(!segment_tap_pin("eth1"));

	listener_cfg_from_conf(&c, "eth2", 1);
	CHK(c.role_pinned && c.role == REAC_ROLE_SLAVE && c.role_intent == REAC_ROLE_INTENT_SLAVE);
	r = REAC_ROLE_MASTER;
	CHK(segment_role_pin("eth2", &r) == 1 && r == REAC_ROLE_SLAVE);

	/* a tap pins NO WIRE ROLE (it presents no end of the pairing) but is still a pin */
	listener_cfg_from_conf(&c, "eth3", 1);
	CHK(c.tap == 1 && c.role_pinned && c.role_intent == REAC_ROLE_INTENT_TAP);
	r = REAC_ROLE_SLAVE;
	CHK(segment_role_pin("eth3", &r) == 0 && r == REAC_ROLE_SLAVE);   /* untouched */
	CHK(segment_tap_pin("eth3"));

	/* `auto` asks for the hunt outright: answered, but not pinned */
	listener_cfg_from_conf(&c, "eth4", 1);
	CHK(!c.role_pinned && c.role_intent == REAC_ROLE_INTENT_AUTO && c.role_layer == REAC_CONF_SEGMENT);
	CHK(segment_role_pin("eth4", &r) == 0);
	CHK(!strcmp(segment_role_source("eth4"), REAC_SEGCONF_FILE));

	/* a segment the file does not name: autodetected, not pinned */
	listener_cfg_from_conf(&c, "eth7", 1);
	CHK(!c.role_pinned && c.role_intent == REAC_ROLE_INTENT_AUTO && c.role_layer == REAC_CONF_NONE);
	CHK(segment_role_pin("eth7", &r) == 0);
	CHK(!strcmp(segment_role_source("eth7"), "autodetected"));
	CHK(segment_role_intent("eth7") == REAC_ROLE_INTENT_AUTO);
	CHK(segment_role_intent("eth1") == REAC_ROLE_INTENT_MASTER);

	/* ignore = yes switches the segment off; others stay on */
	CHK(segment_ignored(NULL, "eth5") == 1);
	CHK(segment_ignored(NULL, "eth1") == 0);
	CHK(segment_ignored(NULL, "eth7") == 0);
	return 0;
}

/* ---- role = box: the row's width, and no second one ----------------------------------- */
static int test_box_row(void)
{
	CHK(home_with_conf("[segment eth1]\nrole = box\nmodel = s4000s\n") == 0);
	clear_env();
	struct listener_cfg c;
	listener_cfg_from_conf(&c, "eth1", 1);
	const struct reac_box_model *m = reac_box_catalogue_by_token("s4000s");
	CHK(m && c.box_model == m && c.role_intent == REAC_ROLE_INTENT_BOX && c.role_pinned);
	CHK(c.box_channels == reac_box_model_upstream_width(m));
	CHK(c.box_channels != REAC_SLAVE_BOX_CHANNELS_DEFAULT);   /* the row's, not the default */

	/* A width key in the env does NOT move it: it is named, not obeyed. */
	setenv("REAC_BOX_CHANNELS_eth1", "8", 1);
	listener_cfg_from_conf(&c, "eth1", 1);
	CHK(c.box_channels == reac_box_model_upstream_width(m));
	unsetenv("REAC_BOX_CHANNELS_eth1");

	/* and a segment that is not a box takes the key, within the legal widths */
	CHK(home_with_conf("[segment eth9]\nrole = master\n") == 0);
	setenv("REAC_BOX_CHANNELS_eth9", "8", 1);
	listener_cfg_from_conf(&c, "eth9", 1);
	CHK(c.box_channels == 8 && c.box_model == NULL);
	setenv("REAC_BOX_CHANNELS_eth9", "7", 1);          /* odd: refused, default kept */
	listener_cfg_from_conf(&c, "eth9", 1);
	CHK(c.box_channels == REAC_SLAVE_BOX_CHANNELS_DEFAULT);
	setenv("REAC_BOX_CHANNELS_eth9", "99", 1);         /* past the box widths */
	listener_cfg_from_conf(&c, "eth9", 1);
	CHK(c.box_channels == REAC_SLAVE_BOX_CHANNELS_DEFAULT);
	clear_env();
	return 0;
}

/* ---- the layered keys of a segment -------------------------------------------------- */
static int test_layered_keys(void)
{
	CHK(home_with_conf("") == 0);
	clear_env();
	struct listener_cfg c;

	/* nothing configured: the interface is its own TX NIC, the first segment keeps BARE node
	 * names, every other segment is named by its interface so two never collide. */
	listener_cfg_from_conf(&c, "eth9", 1);
	CHK(c.rxcfg.kind == REAC_RX_LIVE && !strcmp(c.rxcfg.source, "eth9"));
	CHK(c.tx_if && !strcmp(c.tx_if, "eth9"));
	CHK(c.inst_name == NULL);
	CHK(!strcmp(c.mixer->name, "m200") && c.n_headamps == 0 && !c.src_mac_set);
	CHK(c.box_channels == REAC_SLAVE_BOX_CHANNELS_DEFAULT);
	listener_cfg_from_conf(&c, "eth9", 0);
	CHK(c.inst_name && !strcmp(c.inst_name, "eth9"));

	setenv("REAC_TX_eth9", "eth8", 1);
	setenv("REAC_NAME_eth9", "left", 1);
	setenv("REAC_MIXER_eth9", "m300", 1);
	listener_cfg_from_conf(&c, "eth9", 1);
	CHK(!strcmp(c.tx_if, "eth8") && !strcmp(c.inst_name, "left") && !strcmp(c.mixer->name, "m300"));
	setenv("REAC_MIXER_eth9", "nosuch", 1);             /* unknown: refused, default kept */
	listener_cfg_from_conf(&c, "eth9", 1);
	CHK(!strcmp(c.mixer->name, "m200"));

	/* the bare key is the floor, the per-segment key outranks it */
	clear_env();
	setenv("REAC_MIXER", "m5000", 1);
	listener_cfg_from_conf(&c, "eth9", 1);
	CHK(!strcmp(c.mixer->name, "m5000"));
	setenv("REAC_MIXER_eth9", "m300", 1);
	listener_cfg_from_conf(&c, "eth9", 1);
	CHK(!strcmp(c.mixer->name, "m300"));
	clear_env();

	setenv("REAC_HEADAMP_eth9", "0:phantom:1,3:sens:20", 1);
	listener_cfg_from_conf(&c, "eth9", 1);
	CHK(c.n_headamps == 2 && c.headamps[0].ch == 0 && c.headamps[1].ch == 3 &&
	    c.headamps[1].value == 20);
	setenv("REAC_HEADAMP_eth9", "0:phantom:1,99:pad:1", 1);   /* one bad cell: none applied */
	listener_cfg_from_conf(&c, "eth9", 1);
	CHK(c.n_headamps == 0);
	clear_env();

	setenv("REAC_SRC_MAC_eth9", "02:aa:bb:cc:dd:ee", 1);
	listener_cfg_from_conf(&c, "eth9", 1);
	CHK(c.src_mac_set && c.src_mac[0] == 0x02 && c.src_mac[5] == 0xee);
	setenv("REAC_SRC_MAC_eth9", "02:aa:bb", 1);
	listener_cfg_from_conf(&c, "eth9", 1);
	CHK(!c.src_mac_set);
	clear_env();
	return 0;
}

/* ---- the rate: argv, then the layers, then the best drivable ----------------------------- */
static int test_rate_resolution(void)
{
	CHK(home_with_conf("") == 0);
	clear_env();
	struct listener_cfg c;
	enum reac_conf_layer layer = REAC_CONF_NONE;
	listener_cfg_from_conf(&c, "eth9", 1);

	/* silent layers: the built-in pick, and it says so */
	CHK(listener_resolve_rate(&c, &layer) == reac_rate_best_drivable(REAC_RATE_ALL_BITS));
	CHK(layer == REAC_CONF_BUILTIN);

	setenv("REAC_RATE", "44100", 1);
	CHK(listener_resolve_rate(&c, &layer) == 44100 && layer == REAC_CONF_ENV);
	setenv("REAC_RATE_eth9", "96000", 1);               /* per segment outranks bare */
	CHK(listener_resolve_rate(&c, &layer) == 96000 && layer == REAC_CONF_SEGMENT);

	/* a layer that answers with nonsense is skipped, loudly, not obeyed */
	setenv("REAC_RATE_eth9", "88200", 1);
	setenv("REAC_RATE", "garbage", 1);
	CHK(listener_resolve_rate(&c, &layer) == reac_rate_best_drivable(REAC_RATE_ALL_BITS));
	CHK(layer == REAC_CONF_BUILTIN);
	clear_env();
	return 0;
}

/* ---- deferring to a foreign desk: an AUTO segment defers, a pin or a box never does ---- */
static int test_defers_as_tap(void)
{
	struct reac_hunt hunt;
	memset(&hunt, 0, sizeof hunt);
	hunt.arb.state = REAC_SEGMENT_FOREIGN;
	hunt.arb.rival = REAC_RIVAL_DESK;
	CHK(segment_defers_as_tap(REAC_ROLE_INTENT_AUTO, &hunt) == 1);
	CHK(segment_defers_as_tap(REAC_ROLE_INTENT_MASTER, &hunt) == 1);   /* beside a desk, no fight */
	CHK(segment_defers_as_tap(REAC_ROLE_INTENT_SLAVE, &hunt) == 0);    /* an explicit recorder courts */
	CHK(segment_defers_as_tap(REAC_ROLE_INTENT_TAP, &hunt) == 0);
	CHK(segment_defers_as_tap(REAC_ROLE_INTENT_BOX, &hunt) == 0);      /* the desk is who a box is for */
	CHK(segment_defers_as_tap(REAC_ROLE_INTENT_AUTO, NULL) == 0);

	hunt.arb.rival = REAC_RIVAL_BOX;                    /* a box mastering the wire is joined */
	CHK(segment_defers_as_tap(REAC_ROLE_INTENT_AUTO, &hunt) == 0);
	hunt.arb.rival = REAC_RIVAL_DESK;
	hunt.arb.state = REAC_SEGMENT_NONE;                  /* no foreign master at all */
	CHK(segment_defers_as_tap(REAC_ROLE_INTENT_AUTO, &hunt) == 0);
	return 0;
}

/* A VALUE IS ALL OF ITS TEXT, in a conf key as on the command line. sscanf and atoi stop at
 * the first character they cannot use and say nothing, so REAC_RATE=48000abc ran at 48000,
 * REAC_BOX_CHANNELS=8x made an 8-wide box and REAC_SRC_MAC=<mac>zz was the clean address in
 * front of the junk. */
static int test_conf_values_are_whole(void)
{
	CHK(home_with_conf("") == 0);
	clear_env();
	struct listener_cfg c;
	enum reac_conf_layer layer = REAC_CONF_NONE;

	listener_cfg_from_conf(&c, "eth9", 1);
	setenv("REAC_RATE_eth9", "48000abc", 1);
	CHK(listener_resolve_rate(&c, &layer) == reac_rate_best_drivable(REAC_RATE_ALL_BITS));
	CHK(layer == REAC_CONF_BUILTIN);                     /* skipped, not obeyed */
	clear_env();

	/* a note after the value is text after the value: the conf format has comments on a
	 * line of their own only (packaging/reac-pw.env.example writes them that way) */
	setenv("REAC_RATE_eth9", "48000 # note", 1);
	CHK(listener_resolve_rate(&c, &layer) == reac_rate_best_drivable(REAC_RATE_ALL_BITS));
	CHK(layer == REAC_CONF_BUILTIN);
	clear_env();

	setenv("REAC_BOX_CHANNELS_eth9", "8x", 1);
	listener_cfg_from_conf(&c, "eth9", 1);
	CHK(c.box_channels == REAC_SLAVE_BOX_CHANNELS_DEFAULT);
	clear_env();

	setenv("REAC_SRC_MAC_eth9", "02:aa:bb:cc:dd:eezz", 1);
	listener_cfg_from_conf(&c, "eth9", 1);
	CHK(!c.src_mac_set);
	clear_env();
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= test_role_keys();
	rc |= test_box_row();
	rc |= test_layered_keys();
	rc |= test_rate_resolution();
	rc |= test_defers_as_tap();
	rc |= test_conf_values_are_whole();
	home_remove();
	if (rc == 0)
		printf("test_reac_main_own: ok\n");
	return rc;
}
