// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_knock — THE PROOF THAT A WIRE HAS NO MASTER ON IT, which is the licence to drive.
 *
 * THE DEFECT IT EXISTS AGAINST, measured on the operator's desk 2026-09-08 22:10 with
 * 0.5.0-2: an S-0808 on `enp131s0` and an S-1608 on `enp128s20f0u2`, both freshly
 * powered, both cabled, both NICs carrier up at 100 Mb full — and in five seconds
 * `rx_packets` moved by ZERO on both, with no 0x8819 frame at all in eight seconds of
 * capture. A REAC stagebox in slave mode announces itself by flooding broadcast FILLER
 * for a BOUNDED burst on PHY-up (REAC_FSM_FLOOD_BURST = 5460 frames, ~1.36 s at the 48 k
 * box cadence — reac_fsm.h, byte-verified 2026-07-11) and then goes quiet: it has learnt
 * no master, and it has nowhere to send. A box powered before the daemon started has
 * already spent its flood. It will never speak again on its own.
 *
 * So "the first classifying frame is the gate to SERVE" is a gate a cold segment cannot
 * open, and hearing alone cannot wake a box. `REAC_ROLE_<iface>=master` answers it for a
 * wire the operator pinned (reac_hunt: a pin serves on LINK). This module answers it for
 * a wire nobody pinned, which is every wire on a final-user system's first boot.
 *
 * WHAT IT ANSWERS WITH — corrected on the rig, 2026-09-08, with 0.5.0-3 installed. The
 * first cut sent ONE master announce every two seconds and waited for a reply. It was
 * measured on `enp128s20f0u2` unpinned: the daemon knocked, tx rose by two frames per six
 * seconds, and rx stayed at ZERO for over a minute. A COLD BOX DOES NOT ANSWER A LONE
 * ANNOUNCE. It answers a master that is DRIVING — the continuous probing stream a real
 * desk puts on the wire, which is what the pinned path and 0.4.8 both send and is what
 * brought both rig boxes up within two seconds of link. So the licence this module grants
 * is not "send a frame": it is TAKE THE WIRE, and the master role starts on that port
 * exactly as a pin starts it.
 *
 * WHY DRIVING IS SAFE, which is the whole argument for transmitting at all (operator
 * ruling 2026-09-08). A REAC master transmits CONTINUOUSLY at the wire cadence: one frame
 * per audio slot, ~125 us at 96 k, ~272 us at the slowest rate this daemon serves. A
 * master cannot be present and silent. So a linked wired port that carries NOT ONE REAC
 * frame across REAC_KNOCK_LISTEN_NS has been OBSERVED masterless — proof, not a guess —
 * and only such a port is ever driven. Any frame arriving inside that window cancels the
 * licence outright and hands the wire back to the ordinary hunt, which joins a desk,
 * refuses a stagebox strapped to master, and waits out its window for a box.
 *
 * THE ACCEPTED COST, ruled by the operator 2026-09-08 ("no traffic, no master"): a linked
 * wired interface with nothing on it is driven at the master cadence indefinitely, and on
 * an office LAN that is a REAC stream nothing will ever answer. 0.4.8 did exactly this on
 * every interface it was given. Wireless is excluded from the scan entirely and never
 * reaches this module.
 *
 * THE CANCELLATION IS NOT A LATCH, AND THAT COST NINE MINUTES OF A DESK (2026-09-21
 * 22:17:45, docs/design/notes/2026-09-21-one-stray-frame-pinned-a-wire.md). A frame
 * belonging to a box on ANOTHER interface was misattributed to `enp128s20f0u6`'s sniffer
 * in the instant it opened. `REAC_KNOCK_CANCELLED` was terminal, so that one frame ended
 * the observation for the life of the process: the wire then carried 0 RX packets for nine
 * minutes, the cold S-0808 on the far end was never courted, and the segment sat
 * `listening — role auto` until a human power-cycled the box. Ten minutes later, with no
 * stray frame, the same build took the same wire in 500 ms and the box enrolled in three
 * seconds — the courting works; the latch was the defect.
 *
 * So a cancelled observation RE-OPENS, measured from the last frame heard. The safety
 * argument is not weakened by one word of this: a master fills every audio slot, so a wire
 * that has carried NOT ONE frame across REAC_KNOCK_LISTEN_NS has no master on it, and that
 * is as true of a wire that was heard a minute ago as of one that was never heard at all.
 * A wire that really has a master on it re-cancels thousands of times inside one window
 * and can never re-arm. It is #102's ruling — the hunt asks about the wire as it is NOW —
 * applied to the licence as well as to the trunk verdict.
 *
 * PURE: one clock and one verdict, no sockets, no frames. main.c turns the licence into a
 * reac_hunt verdict and the hunt into a served segment; a wire taken this way KEEPS ITS
 * SNIFFER, because a bet on silence is a bet that has to stay watched — see
 * reac_hunt_silence_proven and main.c's yield.
 */
#ifndef REAC_KNOCK_H
#define REAC_KNOCK_H

#include <stdint.h>

/* HOW LONG A WIRE IS LISTENED TO BEFORE IT MAY BE DRIVEN — the masterless observation.
 *
 * Read off the cadence this daemon already emits, not picked. A master fills every audio
 * slot: `sampleRate/12` frames a second (reac_fsm.h §13p.3 measured 4000 fps at 48 k),
 * so the SLOWEST rate on the closed list, 44.1 k, is 3675 fps = 272 us a slot. Half a
 * second is therefore 1837 consecutive slots at the worst case (and ~4000 at 96 k) in
 * which a present master would have had to transmit and did not.
 *
 * It is also longer than two of main.c's 200 ms hearing polls, so the silence is
 * observed at least twice by the loop that acts on it, and it costs a cold box half a
 * second of extra sleep — which is nothing beside the forever it slept before. */
#define REAC_KNOCK_LISTEN_NS (500ULL * 1000000ULL)

enum reac_knock_state {
	REAC_KNOCK_LISTENING = 0, /* inside the masterless observation; nothing driven */
	REAC_KNOCK_PROVEN,        /* observed masterless: this wire may be taken */
	REAC_KNOCK_CANCELLED,     /* something was heard; the ordinary hunt owns it */
};

/* What the caller must DO this step. DRIVE is a ONE-SHOT: the licence is granted once,
 * and the segment it starts outlives this object. */
enum reac_knock_act {
	REAC_KNOCK_ACT_NONE = 0,  /* nothing to do */
	REAC_KNOCK_ACT_DRIVE,     /* proven masterless: take the wire as MASTER, and say so */
};

struct reac_knock {
	enum reac_knock_state state;
	uint64_t opened_ns;       /* when this wire got carrier and we began listening */
	uint64_t due_ns;          /* when the observation closes */
	uint64_t heard_ns;        /* the last frame heard here (0 = none ever) */
	int granted;              /* the licence was handed out; never handed out twice */
};

/* Begin listening on a wire that has just come up. */
void reac_knock_init(struct reac_knock *k, uint64_t now_ns);

/* A REAC frame was heard on this wire — ANY frame, from a desk, a box or a rival. The
 * licence is cancelled: whatever is out there, the ordinary hunt classifies it and rules
 * on it, and a wire with something on it was never the case this module is for. Safe to
 * call repeatedly, and safe to call after the licence was granted — it does not revoke a
 * segment, which is main.c's own watch over the retained sniffer.
 *
 * THE STAMP IS THE WHOLE OF THE CANCELLATION, and it is what keeps the cancellation from
 * being a LATCH. `now_ns` is the poll's clock; the observation re-opens from it. */
void reac_knock_heard(struct reac_knock *k, uint64_t now_ns);

/* Advance the clock. Returns ACT_DRIVE exactly once, on the first step at or after an
 * observation closes on total silence — the one begun at link-up, or the one a heard
 * frame began where it landed — and ACT_NONE every other time. */
enum reac_knock_act reac_knock_step(struct reac_knock *k, uint64_t now_ns);

#endif /* REAC_KNOCK_H */
