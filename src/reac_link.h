/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * reac_link — is there a CABLE in this interface?
 *
 * One question, no dependencies: neither the TX pacer nor the RX feeder should have to pull the
 * other in to ask the kernel about a link. Both need the answer; issue #95 is what it cost to
 * have neither ask.
 */
#ifndef REAC_LINK_H
#define REAC_LINK_H

/* Does `ifname` currently have CARRIER — a cable in, a peer at the other end?
 *
 * Distinct from "does the interface exist" and from "is it still the one I bound to"
 * (reac_rx.h's predicates, which catch a NIC that vanished or re-enumerated). This catches the
 * commoner case those miss: the interface is perfectly present and THE CABLE IS OUT — same name,
 * same ifindex, no carrier.
 *
 * It matters because of the BOX's state machine, not ours: a stagebox leaves BOOT for ANNOUNCE
 * on PHY LINK-UP and on nothing else — "a data gap does NOT" (reac-firmware-re
 * REAC-PROTOCOL-FROM-SOURCE §10.2). So a master that cannot see carrier return cannot be ready
 * for the one event a box ever enrols on, and a box that went quiet never comes back by itself.
 *
 * Reads `/sys/class/net/<if>/carrier` — the kernel's own answer, no capability, no ioctl, no
 * netlink socket. Returns 1 carrier, 0 no carrier, and -1 UNKNOWN: the file is absent (a NIC that
 * just vanished), unreadable, or the name is too long to be one. **-1 is never "down".** A
 * predicate that cannot see must not be read as a verdict — that is how a probe which stopped
 * working comes to look like a quiet wire.
 *
 * Not RT-safe (a filesystem read); never called from the audio path. */
int reac_link_carrier(const char *ifname);

#endif /* REAC_LINK_H */
