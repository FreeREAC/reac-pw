// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_empty_master: A MASTER THAT STREAMS AT NOBODY STOPS, AND THE WIRE IS KNOCKED ON AGAIN.
 *
 * THE DEFECT (#40, review of PR 41). The master on a wire nobody pinned is started on a box
 * that answered the knock. When that box goes away (unplugged, powered off, switched to M),
 * libreac's master falls back to PROBING and pours its stream onto the empty wire, about
 * 96 Mbit/s at 96 kHz. The first cut waited for three facts at once (not past PROBING, no box
 * recognised, the RX latch clear) and then 10 s more: the latch alone takes 5 s to clear
 * (REAC_SEGMENT_HEARD_QUIET_TICKS), so a box that left cost up to 15 s of stream at nobody.
 *
 * TWO FACTS, EACH ACTED ON AT ONCE.
 *
 * 1. A MASTER THAT HAD A BOX AND GAVE IT UP. libreac's master rides through ~6.5 s of box
 *    silence before it reverts to hunting, as a live M-200i was measured to (libreac
 *    reac_master.h:141-149); that ride-through is the protocol's, and it is not shortened
 *    here. The moment the master goes from past PROBING back to PROBING, the box is gone by
 *    libreac's own verdict and the stream stops on the same poll.
 *
 * 2. A MASTER THAT NEVER GOT PAST PROBING AND HEARS NO BOX. A box with a master sends at
 *    every slot of the wire's cadence: its audio, or the unicast FILLER it interleaves while
 *    it cold-connects (libreac reac_fsm.h:18-22). The pacer counts every frame it receives
 *    from a box (reac_pacer rx_box_frames, read through reac_sink_node_wake_obs). A count that
 *    has not moved for REAC_EMPTY_MASTER_QUIET_NS while the master is still courting is a
 *    wire with no box on it.
 *
 * PURE: the caller reads the two facts once per poll and acts on the answer. */
#ifndef REAC_EMPTY_MASTER_H
#define REAC_EMPTY_MASTER_H

#include <stdint.h>

/* HOW LONG A COURTING MASTER WAITS WITH NOT ONE BOX FRAME. CHOSEN: 2 s. A box with a master
 * is never silent for one slot (250 us at 48 kHz), and the longest gap in the enrolment
 * sequence on record is the 1.489 s between the ENROLL and the box's 04 03 burst (libreac
 * tests/test_master_capture.c:19-20), during which fake_box and the box of that capture go on
 * streaming to their master; 2 s clears it with margin. */
#define REAC_EMPTY_MASTER_QUIET_NS (2ULL * 1000000000ULL)

struct reac_empty_master {
	uint64_t rx_last;          /* the box-frame count at the last step                  */
	uint64_t moved_ns;         /* when it last moved, or when the master opened         */
	int      was_past;         /* the master has been past PROBING since it opened      */
};

/* A master has just opened at `now_ns`; its pacer has counted `rx_box_frames` so far. */
void reac_empty_master_init(struct reac_empty_master *e, uint64_t now_ns,
                            uint64_t rx_box_frames);

/* One poll. `past_probing` is the master's state now (granting or established). Returns 1
 * when the master gave its box up, or has courted for REAC_EMPTY_MASTER_QUIET_NS with not
 * one box frame: stop the stream and listen to the wire again. */
int reac_empty_master_step(struct reac_empty_master *e, uint64_t now_ns,
                           uint64_t rx_box_frames, int past_probing);

#endif /* REAC_EMPTY_MASTER_H */
