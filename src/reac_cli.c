// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_cli.h"

#include "reac_facts_pw.h"   /* the closed rate list, the box widths the messages name */

#include <reac/reac.h>
#include <reac/reac_ctrl.h>
#include <reac/reac_role.h>
#include <reac/reac_master.h>
#include <reac/transport/reac_conf.h>
#include <reac/transport/reac_slave.h>
#include "reac_box_pin.h"
#include "reac_rate_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Parse "aa:bb:cc:dd:ee:ff" into out[6]; returns 0, or -1 on malformed input
 * (text after the sixth pair included). */
int reac_cli_parse_mac(const char *s, uint8_t out[6])
{
	unsigned b[6];
	int used = 0;
	/* %n counts the bytes consumed: anything left over is not a MAC. */
	if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%n",
	           &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &used) != 6 || s[used])
		return -1;
	for (int i = 0; i < 6; i++)
		out[i] = (uint8_t)b[i];
	return 0;
}

/* Split `s` on commas and/or whitespace into up to `max` NUL-terminated
 * tokens, each truncated to IFNAMSIZ-1 bytes. Returns the token count (0 for
 * an empty/NULL/all-separator string). A token beyond `max` is REPORTED and
 * dropped, never silently lost (auto-spine's own bounding rule, applied here
 * to the list of interfaces/segments that names it). */
int reac_cli_split_list(const char *s, char out[][IFNAMSIZ + 1], int max)
{
	if (!s)
		return 0;
	int n = 0;
	const char *p = s;
	while (*p) {
		while (*p == ',' || *p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		const char *start = p;
		while (*p && *p != ',' && *p != ' ' && *p != '\t')
			p++;
		size_t len = (size_t)(p - start);
		if (n >= max) {
			fprintf(stderr, "reac-pw: too many entries in '%s' (max %d); "
			        "'%.*s' is dropped\n", s, max, (int)len, start);
			continue;
		}
		if (len > (size_t)IFNAMSIZ)
			len = IFNAMSIZ;
		memcpy(out[n], start, len);
		out[n][len] = '\0';
		n++;
	}
	return n;
}

/* Parse "CH:PARAM:VALUE" (a master-role --headamp arg, or one cell of a
 * REAC_HEADAMP conf list) into *out. CH is the WIRE channel
 * (0..REAC_HEADAMP_MAX_CH-1); PARAM is phantom|pad|sens; VALUE is 0/1 for
 * phantom|pad and the raw SENS code 0..0x37 for sens. Returns 0, or -1 if
 * malformed / out of range. */
int reac_cli_parse_headamp(const char *s, struct reac_headamp_setting *out)
{
	unsigned ch, val;
	char pstr[16];
	int used = 0;
	if (sscanf(s, "%u:%15[^:]:%u%n", &ch, pstr, &val, &used) != 3 || s[used])
		return -1;
	uint8_t param;
	if (!strcmp(pstr, "phantom"))
		param = REAC_HEADAMP_PHANTOM;
	else if (!strcmp(pstr, "pad"))
		param = REAC_HEADAMP_PAD;
	else if (!strcmp(pstr, "sens"))
		param = REAC_HEADAMP_SENS;
	else
		return -1;
	if (ch >= REAC_HEADAMP_MAX_CH)
		return -1;
	if (param == REAC_HEADAMP_SENS) {
		if (val > REAC_HEADAMP_SENS_MAX)
			return -1;
	} else if (val > 1) {
		return -1;
	}
	out->ch = (uint8_t)ch;
	out->param = param;
	out->value = (uint8_t)val;
	return 0;
}

/* Parse a REAC_HEADAMP conf VALUE: a whitespace/comma separated list of
 * CH:PARAM:VALUE cells (reac_cli_parse_headamp's own grammar) — one conf line
 * replacing the S-1608's 32 individual --headamp flags in a hand-run master.
 * Returns the cell count (0 for an empty list), or -1 on any malformed cell:
 * the WHOLE list is refused rather than applying a partial table silently,
 * the same honesty rule REAC_RATE's layer lookup already follows. */
int reac_cli_parse_headamp_list(const char *s, struct reac_headamp_setting *out, int max)
{
	int n = 0;
	const char *p = s;
	while (*p) {
		while (*p == ',' || *p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		const char *start = p;
		while (*p && *p != ',' && *p != ' ' && *p != '\t')
			p++;
		size_t len = (size_t)(p - start);
		char cell[32];
		if (len >= sizeof cell || n >= max)
			return -1;
		memcpy(cell, start, len);
		cell[len] = '\0';
		if (reac_cli_parse_headamp(cell, &out[n]) != 0)
			return -1;
		n++;
	}
	return n;
}

int reac_cli_parse(int argc, char **argv, struct reac_cli *c)
{
	memset(c, 0, sizeof *c);
	enum reac_conf_layer rate_layer = REAC_CONF_NONE;
	struct reac_rx_cfg rxcfg = { .kind = REAC_RX_PCAP, .source = NULL, .forced_rate = 0,
	                             .pcap_realtime = 1 };
	const char *tx_if = NULL;
	enum reac_role role = REAC_ROLE_MASTER;   /* default master: preserves current behaviour */
	/* `--role tap` is not a value of `role` above — a tap presents no end of the
	 * pairing — so it rides beside it, exactly as listener_cfg.tap does. */
	int role_tap = 0;
	uint8_t src_mac[6] = { 0 };
	int src_mac_set = 0;
	int box_channels = REAC_SLAVE_BOX_CHANNELS_DEFAULT;  /* slave: our input width */
	const struct reac_box_model *pin_model = NULL;  /* --box: pinned NODE geometry     */
	const char *pin_label = NULL;                   /* --box: pinned node label        */
	const char *box_pin_spec = NULL;      /* --box verbatim, for the wire-wins notice
	                                       * that the wire disagreed with it (once) */
	const char *inst_name = NULL;   /* --name: per-instance node suffix (one master/VLAN) */
	/* --headamp CH:PARAM:VALUE (master role, repeatable): the per-channel head-amp
	 * DMX table the master re-asserts to the box. At most one cell per
	 * (channel,param); the table set() overwrites a repeat. */
	struct reac_headamp_setting *headamps = c->headamps;
	int n_headamps = 0;
	const struct reac_mixer_profile *mixer =
		reac_mixer_profile_by_name("m200");   /* master: which desk name we log as */

	/* auto-spine §5: EXTRA `--live` interfaces beyond the template's own (a
	 * repeated flag, or a comma list in one flag). A stable copy is kept here
	 * because the template's own iface below points at argv, which a later
	 * reac_cli_split_list() call must not alias. */
	char *first_iface_buf = c->first_iface_buf;
	char (*extra_ifaces)[IFNAMSIZ + 1] = c->extra_ifaces;
	int n_extra_ifaces = 0;
	int have_live = 0;   /* was --live given at all (vs --pcap, vs neither) */

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--pcap") && i + 1 < argc) {
			rxcfg.kind = REAC_RX_PCAP; rxcfg.source = argv[++i];
		} else if (!strcmp(argv[i], "--live") && i + 1 < argc) {
			char toks[REAC_PW_MAX_LISTENERS][IFNAMSIZ + 1];
			int nt = reac_cli_split_list(argv[++i], toks, REAC_PW_MAX_LISTENERS);
			if (nt == 0) {
				fprintf(stderr, "reac-pw: --live needs at least one interface name\n");
				return REAC_CLI_REFUSED;
			}
			rxcfg.kind = REAC_RX_LIVE;
			int start = 0;
			if (!have_live) {
				/* The FIRST --live (of possibly several): the template's own
				 * interface, byte-identical to today when it is the only one. */
				snprintf(first_iface_buf, sizeof c->first_iface_buf, "%s", toks[0]);
				rxcfg.source = first_iface_buf;
				have_live = 1;
				start = 1;
			}
			/* One slot of REAC_PW_MAX_LISTENERS is always the template's own
			 * interface once this branch runs at all, so extras are bounded to
			 * the rest — checked HERE, not left to the later array-assembly
			 * copy to truncate silently (auto-spine: "a segment beyond the
			 * bound is reported, never silently ignored"). */
			for (int k = start; k < nt; k++) {
				if (n_extra_ifaces >= REAC_PW_MAX_LISTENERS - 1) {
					fprintf(stderr, "reac-pw: too many --live interfaces (max %d); "
					        "'%s' is dropped\n", REAC_PW_MAX_LISTENERS, toks[k]);
					continue;
				}
				snprintf(extra_ifaces[n_extra_ifaces++], sizeof extra_ifaces[0],
				        "%s", toks[k]);
			}
		} else if (!strcmp(argv[i], "--rate") && i + 1 < argc) {
			/* Validate before it reaches the ring depth (sample_rate/4): a
			 * negative/garbage rate underflows to a huge depth, next_pow2
			 * overflows to a 0-slot ring with mask 0xFFFFFFFF, and the first
			 * write scribbles the heap. Accept only sane audio rates — the REAC
			 * world is the 44.1k/48k/88.2k/96k families. */
			int rate = atoi(argv[++i]);
			/* ONLY THREE PACES ARE LEGAL: 44.1, 48 and 96 kHz. A Roland desk
			 * offers exactly these and drives the segment at the one chosen;
			 * anything else is not a slower REAC, it is not REAC, and it would
			 * need RE-PACING between the rig clock and the wire — which reac-pw
			 * cannot do, having no TX resampler. This used to accept anything
			 * from 8000 to 192000 and put it on the wire, a cadence no box can
			 * follow, called configuration. */
			if (!reac_rate_is_closed(rate)) {
				fprintf(stderr, "reac-pw: illegal --rate '%s'. REAC runs at "
				        REACPW_RATES_OR " Hz and nothing else; anything "
				        "else needs re-pacing, which reac-pw cannot do.\n",
				        argv[i]);
				return REAC_CLI_REFUSED;
			}
			rxcfg.forced_rate = rate;
			rate_layer = REAC_CONF_ARGV;
		} else if (!strcmp(argv[i], "--tx") && i + 1 < argc) {
			tx_if = argv[++i];
		} else if (!strcmp(argv[i], "--src-mac") && i + 1 < argc) {
			if (reac_cli_parse_mac(argv[++i], src_mac) != 0) {
				fprintf(stderr, "reac-pw: bad --src-mac '%s' (want aa:bb:cc:dd:ee:ff)\n",
				        argv[i]);
				return REAC_CLI_REFUSED;
			}
			src_mac_set = 1;
		} else if (!strcmp(argv[i], "--role") && i + 1 < argc) {
			if (!strcmp(argv[++i], "tap")) {
				role_tap = 1;
			} else if (reac_role_parse(argv[i], &role) != 0) {
				fprintf(stderr, "reac-pw: unknown --role '%s' (master|slave|tap)\n",
				        argv[i]);
				return REAC_CLI_REFUSED;
			} else {
				role_tap = 0;
			}
		} else if (!strcmp(argv[i], "--mixer") && i + 1 < argc) {
			/* Master role: which desk name reac-pw logs as. Does not set the
			 * wire's pace-code byte; grants are box-defined, so a box locks to
			 * any profile. */
			mixer = reac_mixer_profile_by_name(argv[++i]);
			if (!mixer) {
				fprintf(stderr, "reac-pw: unknown --mixer '%s'; known:", argv[i]);
				for (int k = 0; reac_mixer_profile_at(k); k++)
					fprintf(stderr, " %s (%s)", reac_mixer_profile_at(k)->name,
					        reac_mixer_profile_at(k)->display);
				fprintf(stderr, "\n");
				return REAC_CLI_REFUSED;
			}
		} else if (!strcmp(argv[i], "--box-model") && i + 1 < argc) {
			/* Slave role: pick a FIXED-matrix box model (the matrix is law when we
			 * are a stagebox). Selects the config-announce block, the ASCII name
			 * frame, and the width in one choice. */
			const struct reac_box_model *m = reac_box_catalogue_by_token(argv[++i]);
			if (!m) {
				size_t n; const struct reac_box_model *t = reac_box_catalogue(&n);
				fprintf(stderr, "reac-pw: unknown --box-model '%s'; known:", argv[i]);
				for (size_t k = 0; k < n; k++)
					fprintf(stderr, " %s (%s)", t[k].token, t[k].display);
				fprintf(stderr, "\n");
				return REAC_CLI_REFUSED;
			}
			box_channels = m->in_ch;
		} else if (!strcmp(argv[i], "--box-channels") && i + 1 < argc) {
			box_channels = atoi(argv[++i]);
			if (box_channels < REAC_BRAID_PAIR_CHANNELS || box_channels > REAC_MAX_CHANNELS ||
			    box_channels % REAC_BRAID_PAIR_CHANNELS) {
				fprintf(stderr, "reac-pw: --box-channels must be even, "
				        REACPW_STR(REAC_BRAID_PAIR_CHANNELS) "..%d "
				        "(e.g. " REACPW_STR(REAC_BOX_S0808_IN) " = S-0808, "
				        REACPW_STR(REAC_BOX_S1608_IN) " = S-1608, "
				        REACPW_STR(REAC_BOX_S4000S_3208_IN) " = S-4000S)\n", REAC_MAX_CHANNELS);
				return REAC_CLI_REFUSED;
			}
		} else if (!strcmp(argv[i], "--box") && i + 1 < argc) {
			/* MASTER role: the operator's PIN for a FIXED INSTALLATION — the nodes
			 * exist, named and sized, from boot rather than appearing when the box
			 * powers up (operator, 2026-08-22: reac-pw is a daemon in its own right
			 * and a permanent rig pins its patch; a console is the autodetect case,
			 * and the two are complementary, not alternatives).
			 *
			 * IT PINS NODE GEOMETRY AND LABEL, AND NOTHING ELSE. This flag was retired
			 * on 2026-08-05 because it also pre-fabricated a head-amp ENROLLMENT from
			 * a guess, and a box granted slots it does not own "links, streams audio,
			 * and silently ignores every head-amp record" (902bf39). That door stays
			 * shut: reac_master_set_box remains fed only by what a box declares about
			 * itself. The pin says what to CALL the ports and how many to make; the
			 * wire says what is actually out there, and where they disagree the WIRE
			 * WINS and says so once (reac_box_pin_notice, autodetect watcher above). */
			const char *spec = argv[++i];
			if (reac_box_pin_parse(spec, &pin_model, &pin_label) != 0) {
				size_t nm; const struct reac_box_model *t = reac_box_catalogue(&nm);
				fprintf(stderr, "reac-pw: unknown --box model '%s'; known:", spec);
				for (size_t k = 0; k < nm; k++)
					fprintf(stderr, " %s", t[k].token);
				fprintf(stderr, "  (form: MODEL[:LABEL])\n");
				return REAC_CLI_REFUSED;
			}
			box_pin_spec = spec;
		} else if (!strcmp(argv[i], "--name") && i + 1 < argc) {
			inst_name = argv[++i];   /* per-instance PW node suffix (multi-master) */
		} else if (!strcmp(argv[i], "--headamp") && i + 1 < argc) {
			/* MASTER role: one per-channel head-amp cell the master re-asserts to
			 * the box (declarative/DMX). Repeatable; the table dedups per cell. */
			if (n_headamps >= REAC_CLI_MAX_HEADAMPS) {
				fprintf(stderr, "reac-pw: too many --headamp settings (max %d)\n",
				        REAC_CLI_MAX_HEADAMPS);
				return REAC_CLI_REFUSED;
			}
			if (reac_cli_parse_headamp(argv[++i], &headamps[n_headamps]) != 0) {
				fprintf(stderr, "reac-pw: bad --headamp '%s' (want "
				        "CH:phantom|pad|sens:VALUE; CH 0..%d wire channel; "
				        "VALUE 0/1 for phantom|pad, 0..%d raw code for sens)\n",
				        argv[i], REAC_HEADAMP_MAX_CH - 1, REAC_HEADAMP_SENS_MAX);
				return REAC_CLI_REFUSED;
			}
			n_headamps++;
		} else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
			/* ALREADY RESOLVED, ABOVE. The knob pre-pass reads every --set before
			 * a capability is touched, so all this arm owes is to step over the
			 * KEY=VALUE it consumed. Without it the flag loop fell through to
			 * `usage()` and the daemon EXITED 2 on the very flag --help documents:
			 * `--set` was proven by a unit test that calls reac_knobs_set_argv()
			 * and never ran the binary, so it had never once worked on a command
			 * line (found 2026-09-20, by the first test that passed one). */
			i++;
		} else {
			return REAC_CLI_USAGE;
		}
	}

	if (reac_role_validate(role, tx_if != NULL) != 0) {
		fprintf(stderr, "reac-pw: --role slave needs --tx IFNAME (the REAC NIC for the "
		                "upstream return + handshake)\n");
		return REAC_CLI_REFUSED;
	}
	/* --box declares the box a MASTER serves. As a SLAVE we ARE the box, and our own
	 * width is a different fact with no wire to learn it from: --box-channels /
	 * --box-model. Reject the mix rather than mislead about which one is in force. */
	if (role == REAC_ROLE_SLAVE && pin_model) {
		fprintf(stderr, "reac-pw: --box is a MASTER-role option (it pins the box this "
		                "master serves); a SLAVE's own width is --box-channels or "
		                "--box-model.\n");
		return REAC_CLI_REFUSED;
	}
	/* --headamp drives the box's preamps — only the MASTER commands them; as a slave
	 * WE are the box and receive them (surfaced in the RX log). Reject the mix. */
	if (role == REAC_ROLE_SLAVE && n_headamps > 0) {
		fprintf(stderr, "reac-pw: --headamp is a master-role option (the master commands "
		                "the box's preamps); a slave receives head-amp records, it does "
		                "not send them\n");
		return REAC_CLI_REFUSED;
	}
	c->rate_layer = rate_layer;
	c->rxcfg = rxcfg;
	c->tx_if = tx_if;
	c->role = role;
	c->role_tap = role_tap;
	memcpy(c->src_mac, src_mac, sizeof src_mac);
	c->src_mac_set = src_mac_set;
	c->box_channels = box_channels;
	c->pin_model = pin_model;
	c->pin_label = pin_label;
	c->box_pin_spec = box_pin_spec;
	c->inst_name = inst_name;
	c->n_headamps = n_headamps;
	c->mixer = mixer;
	c->n_extra_ifaces = n_extra_ifaces;
	c->have_live = have_live;
	return 0;
}
