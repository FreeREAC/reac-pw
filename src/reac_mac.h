// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_mac — the single source of the default source MAC every emitting role
 * uses: THE NIC'S OWN HARDWARE ADDRESS, verbatim.
 *
 * Every REAC frame we emit carries this machine's real L2 identity — never a
 * cloned desk MAC and never a Roland-OUI stand-in. Real boxes and desks sync
 * to our real address (rig-verified, S-0808 + S-1608 cold-connect and 48V);
 * a borrowed identity collides with the real device when both are on the wire
 * and makes every capture ambiguous. --src-mac overrides for directed
 * experiments. */
#ifndef REACPW_MAC_H
#define REACPW_MAC_H

#include <stdint.h>

/* The composition rules themselves are libreac's (reac/reac_macaddr.h): reac_mac_compose,
 * reac_mac_roland_standin and the reac_mac48_pack/unpack pair are pure protocol identity and
 * live with the control plane. What stays here is the one thing that touches the machine. */
#include <reac/reac_macaddr.h>

/* Fill `out` with the default source MAC for `ifname` (reads SIOCGIFHWADDR and
 * calls reac_mac_compose). Returns 0 when `out` is the NIC's address, -1 on any
 * failure (ifname NULL/empty, no such device, not an ethernet address), in
 * which case `out` holds the locally-administered fallback. `out` is ALWAYS
 * filled with a usable MAC regardless of the return value.
 *
 * IT IS THE DAEMON'S, NOT THE LIBRARY'S: it opens a datagram socket and issues an ioctl,
 * and libreac opens no sockets. */
int reac_mac_default_src(const char *ifname, uint8_t out[6]);

#endif /* REACPW_MAC_H */
