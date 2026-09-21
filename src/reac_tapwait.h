// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_tapwait — HOW LONG A SIGHTING THE TAP HAS NOT CLASSIFIED MAY HOLD UP THE HUNT.
 *
 * WHY THE WAIT EXISTS. On a physical parent this daemon runs TWO sockets over one netdev:
 * the segment's sniffer (AF_PACKET bound to 0x8819) and the topology tap (ETH_P_ALL with
 * PACKET_AUXDATA, reac_topo.h). They are fed the same frames, and the sniffer CANNOT tell
 * a tagged frame from an untagged one — the kernel hands a bound-protocol socket the frame
 * with the 802.1Q header already gone. So a sighting the tap has not classified YET is a
 * sighting whose VLAN is still unknown, and electing a role on it would serve a TRUNK
 * PARENT: a master on the parent beside the masters on its sub-interfaces, two masters for
 * one box, arrived at through a kernel behaviour. The tap is the authority; the hunt waits
 * for it. On an access port that costs one poll, because the very same frame is already
 * queued on both sockets.
 *
 * WHY THE WAIT IS BOUNDED, AND WHAT IT COST NOT TO BE (2026-09-21 22:17:45; the journal and
 * the measurements are in docs/design/notes/2026-09-21-one-stray-frame-pinned-a-wire.md).
 * The wait was written as two EVER questions — "has anything been heard here at all" and
 * "has the tap classified an untagged frame here, ever" — and on `enp128s20f0u6`, a direct
 * point-to-point cable with one cold S-0808 on it, a frame belonging to the S-1608 on
 * ANOTHER interface was misattributed to the sniffer in the instant it opened. The tap
 * never saw it, so `untagged` stayed 0; the sniffer had heard it, so "heard anything"
 * stayed 1; and the hunt skipped that segment FOR THE LIFE OF THE PROCESS. The wire then
 * carried 0 RX packets for nine minutes with the daemon reporting `listening — role auto`,
 * and eight inputs stayed off the desk until a human power-cycled the box. The same
 * journal second shows the control: `enp131s0` got the identical sighting, its tap kept
 * classifying real untagged frames, and it was mastering three seconds later.
 *
 * So the wait is a fact about the wire NOW — #102's ruling, which `topo_trunk_now` already
 * applies to the trunk verdict and this guard did not. A sighting binds the hunt while it
 * is FRESH; past REAC_TAPWAIT_NS with no untagged classification and no further frame, the
 * wire is silent, and a silent wire is decided by the masterless observation exactly as it
 * is on a clean start (reac_knock.h).
 *
 * THE BAR IS THE DAEMON'S EXISTING ONE, not a new number: REAC_HUNT_WINDOW_NS, the same
 * span `topo_trunk_now` believes a trunk verdict for and the same one the hunt waits out
 * before deciding a silent wire. A real access port re-proves itself thousands of times
 * inside it; a real trunk re-proves its tag just as often, and guard one catches it first.
 *
 * PURE: four inputs and a yes/no, no sockets, no frames, no clock of its own. Main-thread
 * only, like everything that reads a hunt.
 */
#ifndef REAC_TAPWAIT_H
#define REAC_TAPWAIT_H

#include <reac/reac_hunt.h>   /* REAC_HUNT_WINDOW_NS — the daemon's "is this still true" bar */

#include <stdint.h>

/* How long a sighting the tap has not classified goes on binding the hunt. */
#define REAC_TAPWAIT_NS REAC_HUNT_WINDOW_NS

/* What the poll knows about one interface that is not yet a segment. */
struct reac_tapwait_in {
	/** A topology tap of ours is open on this interface (reac_topo_find found it). With
	 *  no tap there is nothing to wait FOR: a VLAN sub-interface has none by design, and
	 *  a tap that could not open is reported and the wire served as an ordinary one. */
	int tapped;
	/** Untagged 0x8819 frames this parent's TAP has classified. Past 0 the tap has
	 *  placed the wire and the sniffer's sightings are about the parent itself. */
	unsigned long untagged;
	/** When this interface's SNIFFER last heard a REAC frame; 0 = never heard. */
	uint64_t last_heard_ns;
	/** The poll's clock, read once at the top of the poll. */
	uint64_t now_ns;
};

/**
 * Does an unclassified sighting still bind the hunt on this interface?
 *
 * 1 = skip this interface this poll, the tap has not placed its frames yet.
 * 0 = the hunt owns it — either the tap has spoken, or nothing has been heard here
 *     recently enough to be evidence about the wire as it is now.
 */
int reac_tapwait_binds(const struct reac_tapwait_in *in);

#endif /* REAC_TAPWAIT_H */
