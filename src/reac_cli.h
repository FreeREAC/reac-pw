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

int reac_cli_parse_mac(const char *s, uint8_t out[6]);
int reac_cli_split_list(const char *s, char out[][IFNAMSIZ + 1], int max);
int reac_cli_parse_headamp(const char *s, struct reac_headamp_setting *out);
int reac_cli_parse_headamp_list(const char *s, struct reac_headamp_setting *out, int max);

#endif
