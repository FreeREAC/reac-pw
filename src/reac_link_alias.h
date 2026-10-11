/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/* reac_link_alias — read and write one netdev's interface alias over rtnetlink.
 *
 * The mechanism only: `ip link set dev <name> alias <text>` and its read-back, plus the
 * link kind (`vlan`, `veth`, ... or empty for a physical NIC) from the same answer. No
 * policy lives here; reac_served_mark.h decides what to write. Kept apart from the policy
 * so its test can replace these two doors at link time and touch no device. */
#ifndef REAC_LINK_ALIAS_H
#define REAC_LINK_ALIAS_H

#include <stddef.h>

/* Read `name`'s link kind into `kind` (empty when the netdev has none, which is what a
 * physical NIC answers) and its alias into `alias` (empty when it has none). Either
 * buffer may be NULL. Returns 0, or -errno (-ENODEV for no such interface). */
int reac_link_alias_read(const char *name, char *kind, size_t kind_n,
                         char *alias, size_t alias_n);

/* Set `name`'s alias to `alias`; an empty string removes it. Needs CAP_NET_ADMIN, the
 * same capability the daemon already uses to bring a link down and up. Returns 0, or
 * -errno. */
int reac_link_alias_write(const char *name, const char *alias);

#endif
