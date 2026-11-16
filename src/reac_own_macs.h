/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/* reac_own_macs — THE HOST'S OWN ADDRESSES ARE NEVER A RIVAL (#8).
 *
 * A NIC on a mirrored switch port hears the REAC master frames this host sends from another
 * of its interfaces. The sniffer's classifier is told the address of the NIC it listens on,
 * but not the others, so the copies read as a foreign desk whose address is ours, and the
 * segment gave up the master role for a rival that was ourselves. The set here is every
 * address of every interface of this host, read from the kernel's netdev table; a frame
 * sourced from one of them never reaches the hunt. */
#ifndef REAC_OWN_MACS_H
#define REAC_OWN_MACS_H

#include <stddef.h>
#include <stdint.h>

#define REAC_OWN_MACS_MAX 128
#define REAC_OWN_MACS_SYS "/sys/class/net"

struct reac_own_macs {
	unsigned n;
	uint8_t  mac[REAC_OWN_MACS_MAX][6];
};

/* Replace `s` with the address of every interface under `netdir` (NULL: REAC_OWN_MACS_SYS),
 * one `<name>/address` file each; all-zero addresses (loopback) are skipped. Returns the
 * number read, or -1 when `netdir` cannot be opened, leaving `s` as it was. */
int reac_own_macs_read(struct reac_own_macs *s, const char *netdir);

int reac_own_macs_has(const struct reac_own_macs *s, const uint8_t mac[6]);

enum reac_own_echo {
	REAC_OWN_ECHO_NONE = 0,   /* not ours: offer it to the hunt                       */
	REAC_OWN_ECHO_FIRST,      /* ours, first on this interface: drop it and say so    */
	REAC_OWN_ECHO_AGAIN,      /* ours, already said on this interface: drop it        */
};

/* Judge one received frame by its source address. `said` is the interface's own flag,
 * set on the first echo so the report is made once per interface and not per frame. */
enum reac_own_echo reac_own_echo_screen(const struct reac_own_macs *s, int *said,
                                        const uint8_t *frame, size_t n);

#endif
