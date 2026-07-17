// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_mac — the single source of the stand-in source MAC every emitting role
 * uses. REAC frames must carry a Roland OUI (00:40:ab): a real master may
 * validate that the devices on its segment are Roland (untested to reject a
 * non-Roland source), so we keep the OUI. But the HOST part (the last three
 * bytes) must never equal a real box's, or captures become ambiguous and — worse
 * — two devices could answer to the same L2 address on the wire. So we derive the
 * host part from OUR OWN NIC's hardware address (SIOCGIFHWADDR), which is unique
 * to this machine and cannot collide with any Roland box (e.g. a real S-1608 at
 * 00:40:ab:c4:80:41). --src-mac still overrides everything for explicit control. */
#ifndef REAC_MAC_H
#define REAC_MAC_H

#include <stdint.h>

/* The Roland OUI, kept on every emitted frame's source. */
extern const uint8_t reac_roland_oui[3];

/* Compose a stand-in source MAC from an interface hardware address: the Roland
 * OUI + the last three bytes of `hwaddr`. PURE (no I/O) so the derivation is
 * unit-testable without touching a NIC. `hw_family` is the sa_family reported by
 * SIOCGIFHWADDR; only ARPHRD_ETHER yields a derived host part — anything else
 * (loopback, no hwaddr) falls back to a FIXED host part that is deliberately NOT
 * any known Roland box host part, so even the fallback cannot collide. Returns 0
 * when the host part was derived from `hwaddr`, -1 when the fallback was used. */
int reac_mac_compose(int hw_family, const uint8_t hwaddr[6], uint8_t out[6]);

/* Fill `out` with the stand-in source MAC for `ifname` (reads SIOCGIFHWADDR and
 * calls reac_mac_compose). Returns 0 when the host part was derived from the NIC,
 * -1 on any failure (ifname NULL/empty, no such device, not an ethernet address),
 * in which case `out` holds the Roland OUI + the fixed fallback host part. `out`
 * is ALWAYS filled with a usable Roland-OUI MAC regardless of the return value. */
int reac_mac_default_src(const char *ifname, uint8_t out[6]);

#endif /* REAC_MAC_H */
