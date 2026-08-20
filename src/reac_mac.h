// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_mac — the single source of the default source MAC every emitting role
 * uses: THE NIC'S OWN HARDWARE ADDRESS, verbatim.
 *
 * Every REAC frame we emit is OURS — it carries this machine's real L2
 * identity, never a cloned desk MAC and never a dressed-up stand-in. Two
 * theories died to get here, both refuted by rig evidence (operator,
 * 2026-08-20):
 *   - impersonating a captured desk's MAC (the old master default) — real
 *     boxes sync to us on our own address;
 *   - keeping a Roland OUI on a stand-in host part (the old slave default,
 *     "a real master may validate the OUI", never demonstrated) — real desks
 *     grant us on our own address too.
 * A borrowed identity is worse than none: it collides with the real device
 * when both are on the wire and makes every capture ambiguous.
 *
 * --src-mac still overrides everything for explicit control (the one remaining
 * legitimate use of a foreign address: directed experiments). */
#ifndef REAC_MAC_H
#define REAC_MAC_H

#include <stdint.h>

/* Compose the default source MAC from an interface hardware address: the
 * address VERBATIM. PURE (no I/O) so the rule is unit-testable without a NIC.
 * `hw_family` is the sa_family reported by SIOCGIFHWADDR; only ARPHRD_ETHER
 * yields the NIC address — anything else (loopback, no hwaddr) falls back to a
 * FIXED locally-administered address (02:...) that no real device carries, so
 * even the fallback cannot collide. Returns 0 when `out` is the NIC address,
 * -1 when the fallback was used. */
int reac_mac_compose(int hw_family, const uint8_t hwaddr[6], uint8_t out[6]);

/* Fill `out` with the default source MAC for `ifname` (reads SIOCGIFHWADDR and
 * calls reac_mac_compose). Returns 0 when `out` is the NIC's address, -1 on any
 * failure (ifname NULL/empty, no such device, not an ethernet address), in
 * which case `out` holds the locally-administered fallback. `out` is ALWAYS
 * filled with a usable MAC regardless of the return value. */
int reac_mac_default_src(const char *ifname, uint8_t out[6]);

#endif /* REAC_MAC_H */
