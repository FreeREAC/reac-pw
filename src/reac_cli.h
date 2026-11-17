// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_cli -- the parsers main() reads its flags and conf lists with.
 *
 * PURE: text in, values out, no PipeWire, no socket, no process state. They lived as
 * statics in main.c, where the only thing that could call them was the whole daemon;
 * here a unit test compiles them alone (tests/test_reac_cli.c). */
#ifndef REAC_CLI_H
#define REAC_CLI_H

#include <stdint.h>
#include <net/if.h>            /* IFNAMSIZ */
#include <reac/reac_headamp_tx.h>   /* struct reac_headamp_setting */
#include <reac/reac_role.h>
#include <reac/reac_ctrl.h>         /* struct reac_box_model */
#include <reac/transport/reac_conf.h>   /* enum reac_conf_layer */
#include <reac/transport/reac_rx.h>     /* struct reac_rx_cfg */

/* Bounded, per 2026-08-20-reac-auto-spine ("a segment beyond the bound is reported, never
 * silently ignored") — this rig needs 2; 8 is headroom for a bigger trunk without inviting
 * an unbounded array. The first --live plus the extras fit under it. */
#define REAC_PW_MAX_LISTENERS 8

int reac_cli_parse_int(const char *s, int *out);
int reac_cli_parse_mac(const char *s, uint8_t out[6]);
int reac_cli_split_list(const char *s, char out[][IFNAMSIZ + 1], int max);
int reac_cli_parse_headamp(const char *s, struct reac_headamp_setting *out);
int reac_cli_parse_headamp_list(const char *s, struct reac_headamp_setting *out, int max);

/* THE COMMAND LINE, AS ONE VALUE. These are the flags main() always had; they describe
 * ONE segment (the first --live, or --pcap) and nothing else -- the segments the daemon
 * hears never read them. `rxcfg.source` points into this struct (first_iface_buf) or at
 * argv, so the struct must outlive its use and must not be copied before then. */
#define REAC_CLI_MAX_HEADAMPS (REAC_HEADAMP_MAX_CH * REAC_HEADAMP_NPARAMS)

struct reac_mixer_profile;

struct reac_cli {
	enum reac_conf_layer rate_layer;     /* REAC_CONF_ARGV once --rate was given */
	struct reac_rx_cfg rxcfg;            /* --pcap / --live / --rate */
	const char *tx_if;                   /* --tx */
	enum reac_role role;                 /* --role master|slave (default master) */
	int role_tap;                        /* --role tap rides beside `role` */
	uint8_t src_mac[6];                  /* --src-mac */
	int src_mac_set;
	int box_channels;                    /* --box-channels / --box-model: a slave's own width */
	const struct reac_box_model *pin_model;   /* --box MODEL[:LABEL] */
	const char *pin_label;
	const char *box_pin_spec;            /* --box verbatim */
	const char *inst_name;               /* --name */
	struct reac_headamp_setting headamps[REAC_CLI_MAX_HEADAMPS];   /* --headamp, repeatable */
	int n_headamps;
	const struct reac_mixer_profile *mixer;   /* --mixer (default m200) */
	char first_iface_buf[IFNAMSIZ + 1];  /* the first --live, copied */
	char extra_ifaces[REAC_PW_MAX_LISTENERS][IFNAMSIZ + 1];   /* the rest */
	int n_extra_ifaces;
	int have_live;                       /* --live given at all */
};

/* Exit codes of reac_cli_parse: 0 parsed; REAC_CLI_REFUSED (2) a flag or a combination was
 * refused and the reason is already on stderr; REAC_CLI_USAGE an unknown flag, for which the
 * CALLER prints its usage text and exits 2. */
#define REAC_CLI_REFUSED 2
#define REAC_CLI_USAGE   1

int reac_cli_parse(int argc, char **argv, struct reac_cli *c);

#endif
