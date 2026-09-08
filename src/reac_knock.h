// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_knock — WAKING A COLD STAGEBOX ON A WIRE NOBODY CONFIGURED.
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
 * a wire nobody pinned, which is every wire on a final-user system's first boot: it
 * KNOCKS — one master announce, periodically, until something answers.
 *
 * WHY THE KNOCK CANNOT RACE A MASTER, which is the whole safety argument (operator
 * ruling 2026-09-08). A REAC master transmits CONTINUOUSLY at the wire cadence: one
 * frame per audio slot, ~125 µs at 96 k, ~272 µs at the slowest rate this daemon serves.
 * A master cannot be present and silent. So a linked wired port that carries NOT ONE
 * REAC frame across REAC_KNOCK_LISTEN_NS has been OBSERVED masterless — proof, not a
 * guess — and a knock is only ever emitted onto such a port. The first frame heard, from
 * that instant on, stops the knocking: a desk's stream means we are late to a master's
 * wire and become its slave without ever fighting it (the arbitration's observe-then-act
 * law), a box's answer means we take the wire, a box strapped to master is refused as
 * today. Everything after the first frame is the existing hunt's decision, unchanged;
 * this module only ends the silence that kept the hunt from ever having evidence.
 *
 * THE ACCEPTED COST, ruled by the operator 2026-09-08: on a linked wired interface with
 * no REAC traffic, this daemon puts ONE small 0x8819 broadcast frame on the wire every
 * REAC_KNOCK_PERIOD_NS, indefinitely. On an office LAN that is an unsolicited Roland-OUI
 * frame every two seconds that nothing will ever answer. That is the price of a box that
 * wakes with no configuration, and it is the one the operator chose. Wireless interfaces
 * are excluded from the scan entirely and never reach this module.
 *
 * PURE, like reac_hunt: state, two clocks and a verdict, no sockets. main.c owns the TX.
 * One knock per passive sniffer, alive only until its segment is served or its link goes.
 */
#ifndef REAC_KNOCK_H
#define REAC_KNOCK_H

#include <stdint.h>

/* HOW LONG A WIRE IS LISTENED TO BEFORE THE FIRST KNOCK — the masterless observation.
 *
 * Read off the cadence this daemon already emits, not picked. A master fills every audio
 * slot: `sampleRate/12` frames a second (reac_fsm.h §13p.3 measured 4000 fps at 48 k),
 * so the SLOWEST rate on the closed list, 44.1 k, is 3675 fps = 272 µs a slot. Half a
 * second is therefore 1837 consecutive slots at the worst case (and ~4000 at 96 k) in
 * which a present master would have had to transmit and did not.
 *
 * It is also longer than two of main.c's 200 ms hearing polls, so the silence is
 * observed at least twice by the loop that acts on it, and it costs a cold box half a
 * second of extra sleep — which is nothing beside the forever it slept before. */
#define REAC_KNOCK_LISTEN_NS (500ULL * 1000000ULL)

/* THE KNOCK PERIOD — one master announce every two seconds.
 *
 * The number comes from the box's own cold-connect timing, which this codebase already
 * carries: REAC_FSM_FLOOD_BURST = 5460 frames ≈ 1.36 s at the 48 k box cadence is how
 * long a box takes to announce and hand off to its unicast cold-connect grid. A knock
 * period shorter than that would put a second knock inside the answer round the first
 * one just started. 2 s is the next whole second above it; it is twice the master's own
 * 1 Hz announce cadence (`reac_master.c`, announce_tick >= fps), so a knock can never be
 * mistaken for a master's cadence by anything counting frames; and three of them fit
 * inside no useful window, which is why the wire is DECIDED by the hunt on the first
 * frame and never by a knock count. `tests/test_reac_knock.c` asserts the flood relation
 * mechanically, so moving REAC_FSM_FLOOD_BURST breaks the build's test rather than the
 * reasoning silently. */
#define REAC_KNOCK_PERIOD_NS (2ULL * 1000000000ULL)

/* Bounded jitter on each period, so two daemons brought up together on one switch do not
 * lock step and knock in the same millisecond forever. An eighth of the period (250 ms)
 * is enough to break a tie and small enough that the period is still honestly "every two
 * seconds" in the journal line. Deterministic per host: seeded off our own MAC, so a
 * capture is reproducible and no test has to tolerate a random schedule. */
#define REAC_KNOCK_JITTER_NS (REAC_KNOCK_PERIOD_NS / 8)

enum reac_knock_state {
	REAC_KNOCK_LISTENING = 0, /* inside the masterless observation; nothing sent */
	REAC_KNOCK_KNOCKING,      /* observed masterless: one announce per period */
	REAC_KNOCK_STOPPED,       /* something REAC was heard; the hunt owns it now */
};

/* What the caller must DO this step. The two SEND kinds are separated because the first
 * knock is the one that earns a journal line and the rest must never log — a line every
 * two seconds forever is a log nobody reads. */
enum reac_knock_act {
	REAC_KNOCK_ACT_NONE = 0,  /* nothing to do */
	REAC_KNOCK_ACT_BEGIN,     /* send, and say we have started knocking */
	REAC_KNOCK_ACT_SEND,      /* send, silently */
	REAC_KNOCK_ACT_END,       /* stop, and say why (reac_knock_stop_reason) */
};

struct reac_knock {
	enum reac_knock_state state;
	uint64_t opened_ns;       /* when this wire got carrier and we began listening */
	uint64_t next_ns;         /* when the next knock is due */
	unsigned long sent;       /* knocks put on the wire, for the closing line */
	int end_said;             /* the ACT_END line is a one-shot */
	uint32_t rng;             /* the jitter's own state, seeded from our MAC */
};

/* Begin listening on a wire that has just come up. `our_mac` is this NIC's address and
 * seeds the jitter only. */
void reac_knock_init(struct reac_knock *k, const uint8_t our_mac[6], uint64_t now_ns);

/* A REAC frame was heard on this wire — ANY frame, from a desk, a box or a rival. The
 * knock is over from this instant: whatever is out there, the hunt classifies it and
 * decides, and nothing more of ours goes on the wire until the segment is served. Safe
 * to call repeatedly and safe to call before the first knock. */
void reac_knock_heard(struct reac_knock *k);

/* Advance the clock and say what to do. Returns ACT_BEGIN or ACT_SEND on the steps that
 * must put one master announce on the wire, ACT_END exactly once after
 * reac_knock_heard(), and ACT_NONE otherwise. */
enum reac_knock_act reac_knock_step(struct reac_knock *k, uint64_t now_ns);

/* Why the knocking ended, for the closing journal line. Only ever one reason today —
 * something answered — and it is a function rather than a literal so a second reason
 * (a serve, a link loss) has somewhere to go without the caller inventing prose. */
const char *reac_knock_stop_reason(const struct reac_knock *k);

#endif /* REAC_KNOCK_H */
