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

/* THE ONE EXCEPTION, AND THE RIG THAT FORCED IT (0.5.6, 2026-09-09). The law above is a
 * decision with rig evidence behind it and it still holds everywhere it was made for: a
 * MASTER announces from this machine's real address and real boxes cold-connect to it.
 *
 * A stagebox on M is the case it was not made for. Joining one, the daemon is not a desk
 * announcing itself — it is a BOX asking another box to enrol it, and every box that has
 * ever been granted on this rig announced from a Roland OUI (`00:40:ab:…`). With the NIC's
 * own `00:14:5c:…` in the source, the S-0808 was sent a correct cold-connect burst four
 * times over and echoed nothing, its lamp blinking, for sixty seconds (`rig-0.5.6-enrol.pcap`).
 * That is not proof the OUI is the reason — the announce's own selector was wrong in the
 * same capture — but it is the one field we can make match a granted box at no cost, and
 * the rig settles the pair together.
 *
 * IT CANNOT COLLIDE. The OUI is Roland's; the low three bytes are THIS NIC's, so two hosts
 * on one wire stay distinct and a capture still says which machine spoke. `--src-mac`
 * overrides it exactly as it overrides the default, and nothing else in the daemon uses it:
 * the master role, the desk-slave role and every other emitter keep the address verbatim.
 *
 * AND IT COSTS A PROMISCUOUS SOCKET. A box unicasts to the address it was announced from,
 * and the NIC's hardware filter drops a unicast to an address the card does not own — the
 * same reason reac_pacer sets PACKET_MR_PROMISC for the master role. reac_slave sets it
 * whenever its source is not the NIC's own, or this would trade a box that will not grant
 * for a box whose grant we cannot hear.
 *
 * PURE, so the composition is unit-testable without a NIC. Returns 0 always; `out` is
 * `00:40:ab` followed by hwaddr[3..5]. */
int reac_mac_roland_standin(const uint8_t hwaddr[6], uint8_t out[6]);

/* Pack six MAC bytes into the low 48 bits of a uint64_t, big-endian (byte 0
 * highest), and back out again.
 *
 * IT EXISTS SO A MAC CAN CROSS A THREAD AS ONE ATOMIC. reac_slave's engine
 * thread learns the master's address and the main loop publishes it on the
 * segment's node properties; six loose bytes read across that boundary are a
 * torn read nobody synchronises, and half a MAC is a WRONG answer rather than a
 * stale one. Packed, it is one relaxed store and one relaxed load.
 *
 * 0 means NO MAC — the all-zero address is not one any device carries, and it is
 * what an unlearned master publishes "none" from. PURE. */
uint64_t reac_mac48_pack(const uint8_t mac[6]);
void reac_mac48_unpack(uint64_t packed, uint8_t out[6]);

#endif /* REAC_MAC_H */
