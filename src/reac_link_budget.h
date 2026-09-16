/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/* reac_link_budget — how much of a physical link a REAC master costs, and whether
 * another one fits on it.
 *
 * THE FAULT THIS EXISTS FOR, MEASURED 2026-09-16 ON THE DESK. `enp131s0` is a
 * 100 Mbit/s port carrying FOUR declared master segments: the untagged one plus
 * VLANs 11, 12 and 13. A REAC master's downstream is a fixed 1492-byte broadcast
 * at sample_rate/12 packets per second, so each of them offers ~97 Mbit/s and the
 * four together offer 387 Mbit/s onto a 100 Mbit/s wire. The link ran at 99.6
 * Mbit/s — saturated — and the port's etf qdisc discarded the rest as overlimit:
 * 23 784 packets per second, 75% of everything, spread evenly across all four
 * segments. Nothing said so. Each segment's master stream reached the wire at
 * ~2 000 pps where the protocol needs 8 000, the once-a-second invitations were
 * thinned in the same proportion, and an S-1608 in slave mode on that port sat
 * with rx=0 for hours because it never saw a master stream it could lock to.
 *
 * Three of those four segments had no box on them at all (`reac.box.mac: none`):
 * the wire was being spent on segments nobody was listening to.
 *
 * A MASTER THAT CANNOT FIT REFUSES, AND SAYS SO. Delivering a quarter of a stream
 * is the silent-clamp defect: every other sign of health stays green — the node is
 * there, the pacer counts its frames, the qdisc's drops are on a counter nobody
 * reads — while no box can ever sync. The daemon knows the link's speed
 * (`/sys/class/net/<dev>/speed`) and knows exactly what a master costs, so it can
 * say "this one does not fit" instead of transmitting into a full pipe.
 *
 * PURE: no I/O, no netlink, no PipeWire. The caller reads the speed and the
 * already-committed load and asks.
 */
#ifndef REAC_LINK_BUDGET_H
#define REAC_LINK_BUDGET_H

#include <stdint.h>

/* Ethernet's per-frame cost BESIDE the frame: 8 B preamble + SFD, 4 B FCS and the
 * 12 B interframe gap. A budget that counts only the frame under-reports a REAC
 * master by ~1.6% at 1492 B — small here, and wrong in the direction that lets one
 * more stream on. */
#define REAC_LINK_WIRE_OVERHEAD_BYTES 24

/* THE BUDGET IS THE LINK ITSELF, and there is no headroom fraction on purpose. REAC is
 * designed to FILL its wire: one 96 kHz master is 97.0 Mbit/s of a 100 Mbit/s port, 97%,
 * and that is the normal, working case — the rig has run it for weeks. A rule that
 * reserved even 5% would refuse the only configuration this protocol has at its top rate,
 * which is the opposite of the defect being fixed. What the budget refuses is a SECOND
 * stream on a port that is already carrying one, because two never fit at any rate. */
#define REAC_LINK_BUDGET_NUM 100
#define REAC_LINK_BUDGET_DEN 100

/* What a stream of `pps` frames of `frame_bytes` each costs on the wire, in kbit/s.
 * kbit rather than Mbit because a 96 kHz master is 97 Mbit/s and the difference
 * between two of them and one is not expressible in whole Mbit. 0 for a zero rate. */
uint64_t reac_link_cost_kbit(unsigned pps, unsigned frame_bytes);

/* The packet rate a REAC master's downstream runs at: 12 samples per frame, at every
 * rate (reac.h's own geometry — 3675 / 4000 / 8000 pps at 44.1 / 48 / 96 kHz). 0 for
 * a sample rate of 0. */
unsigned reac_link_master_pps(unsigned sample_rate);

/* Does a new stream costing `want_kbit` fit on a `link_mbit` link that has already
 * committed `used_kbit`?
 *
 * Returns 1 when it fits, 0 when it does not. **A LINK SPEED OF 0 ALWAYS FITS**:
 * `/sys/class/net/<dev>/speed` answers -1 (and the reader 0) for a link that is down,
 * for a virtual device, and inside a netns where the file is unreadable — and refusing
 * a master because we could not READ a number would turn every veth bench and every
 * unplugged port into a silent segment. An unknown link is not a full one; absence is
 * reported by the caller, never treated as a refusal.
 */
int reac_link_budget_fits(unsigned link_mbit, uint64_t used_kbit, uint64_t want_kbit);

#endif /* REAC_LINK_BUDGET_H */
