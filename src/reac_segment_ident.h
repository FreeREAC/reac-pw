// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_segment_ident — WHAT A SEGMENT IS, in whichever role it is running, so a
 * console addresses the SEGMENT and never a node kind
 * (docs/design/specs/2026-08-20-reac-master-arbitration.md §1 and §8, in the
 * openmixer tree; docs/SLAVE-EMULATION-SCOPE.md W1).
 *
 * THE DEFECT THIS CLOSES. A segment's identity was only ever recoverable by
 * PARSING a node name — `reac-playback.<inst>` minus the prefix. That works for
 * exactly one role, because the prefix IS the role: a master has a reac-playback
 * node and a slave has none (main.c builds reac_sink_node for the master branch
 * alone), so the same segment is addressable as a mixer and unaddressable as a
 * recorder. The console could therefore drive a segment TO recorder and never
 * back. Recovering a fact by parsing a name is the defect the port contract
 * already names (2026-08-21-reac-adapter-pace-and-port-contract.md: constructing
 * a name to address a node is fine, parsing one to recover a fact is not); here
 * it also happens to be role-dependent, which is what made it fatal rather than
 * merely untidy. REAC_PROP_SEGMENT declares the identity instead.
 *
 * EXACTLY ONE NODE PER SEGMENT CARRIES IT, and that node is the segment's DOOR —
 * the one that accepts `reac.cfg.role` / `reac.cfg.rate` and publishes the answer:
 * reac-playback in the master role, reac-capture in the slave role. That is not a
 * convenience: it is what lets a reader key a row on this value and get one row
 * per segment BY CONSTRUCTION rather than by a de-duplication rule it has to get
 * right. A master's reac-capture node deliberately carries no reac.segment — it
 * is the same segment's audio, but it is not its doorway, and one store has one
 * writer.
 *
 * PURE. No PipeWire, no socket, no clock, no state kept between calls beyond the
 * caller's own record — the same shape reac_role_swap, reac_role_cfg and
 * reac_link_state keep, and offline-testable the same way
 * (tests/test_reac_role_swap.c).
 *
 * THE ANSWER SET IS DERIVED, NEVER ASSERTED. A slave publishes only what its own
 * engine can witness, in the vocabulary reac_arbitration already owns — the
 * strings here come from reac_segment_master_name / reac_pace_source_name /
 * reac_rival_kind_name / reac_rival_refusal, never from a second copy of them.
 * What a slave cannot witness is ABSENT rather than defaulted: no
 * reac.discovery.* (a slave runs no disco classifier), no reac.link-state or
 * reac.box-* (a different FSM entirely), no reac.rate.drivable (a slave drives no
 * pace — its cadence is the desk's), no reac.headamp.* (a box is told what its
 * preamps do; it never tells its desk). Absence is a fact. */
#ifndef REAC_SEGMENT_IDENT_H
#define REAC_SEGMENT_IDENT_H

#include <stdint.h>

#include "reac_mac.h"   /* reac_mac48_pack/unpack — the master MAC as ONE atomic */

/* The segment's own name, on the node that carries its door. The value is the
 * per-instance name (`--name` / `REAC_NAME`), or REAC_SEGMENT_NAME_DEFAULT for a
 * lone unnamed segment — the same two cases the node names already encode, now
 * said rather than spelled into a prefix. */
#define REAC_PROP_SEGMENT        "reac.segment"
#define REAC_SEGMENT_NAME_DEFAULT "default"

/* The identity for an instance name. `inst` NULL or empty is the bare, unnamed
 * segment. Returns a string owned by the caller's `inst` or by this module's own
 * literal — never allocated. */
const char *reac_segment_name(const char *inst);

/* ---- IS A MASTER STILL BEING HEARD ON THIS WIRE? ------------------------- *
 *
 * A slave's evidence that a foreign master exists is that its downstream frames
 * KEEP ARRIVING. The RX gate in the slave role accepts the 40-channel master
 * downstream and nothing else (reac_rx.c's gate_accepts under
 * REAC_RX_ACCEPT_DOWNSTREAM), so a moving `frames_ok` IS the geometry evidence
 * as well as the presence evidence — which is why reac_rival_kind_from_channels
 * can be asked about it below rather than a second classification invented here.
 *
 * IT MUST DECAY, and that is the whole reason this is a latch and not a
 * comparison. A cumulative counter never goes back down, so "we once decoded a
 * master frame" would keep reporting a desk that was unplugged an hour ago —
 * exactly the staleness reac_disco ages its sightings to avoid ("a master heard
 * once and gone is not a master"). The caller steps this on its own publish
 * timer and the latch clears after REAC_SEGMENT_HEARD_QUIET_TICKS ticks with no
 * new frame. */
struct reac_segment_heard {
	uint64_t last_frames;   /* the cumulative count at the previous step  */
	int      quiet_ticks;   /* consecutive steps with no new frame        */
	int      heard;         /* 1 while a master is being heard right now  */
};

/* Ticks of silence before a heard master is no longer claimed. main.c steps this
 * from the 200 ms poll, so 25 ticks is 5 s — deliberately reac_disco's own
 * withdrawal bar, so a desk and a box stop being reported on the same clock. */
#define REAC_SEGMENT_HEARD_QUIET_TICKS 25

/* Seed the latch: nothing heard yet, whatever the counter already reads. Called
 * once per listener open, so a segment that re-opens does not inherit the
 * previous engine's evidence. */
void reac_segment_heard_init(struct reac_segment_heard *h, uint64_t frames_ok);

/* One step against the RX's cumulative accepted-frame count. Returns the latch's
 * new value (1 = a master is being heard), which is also left in `h->heard`. */
int reac_segment_heard_step(struct reac_segment_heard *h, uint64_t frames_ok,
                            int quiet_limit);

/* ---- THE SLAVE'S PUBLISHED ANSWER SET ------------------------------------ *
 *
 * Every member is the exact string that goes onto the corresponding property, so
 * the caller composes a pw_properties dict and nothing more. Sized to the
 * longest member of each vocabulary. */
struct reac_segment_answer {
	char master_state[8];    /* REAC_PROP_MASTER_STATE:    us | foreign | none      */
	char master_mac[24];     /* REAC_PROP_MASTER_MAC:      aa:bb:… or "none"        */
	char pace_source[16];    /* REAC_PROP_PACE_SOURCE:     foreign-master | free-run */
	char conflict[2];        /* REAC_PROP_MASTER_CONFLICT: always "0" — see below   */
	char rival_kind[8];      /* REAC_PROP_RIVAL_KIND:      desk | none              */
	char refusal[24];        /* REAC_PROP_REFUSAL:         none                     */
	char rate[8];            /* REAC_PROP_RATE:            the wire pace, decimal Hz */
};

/* Fill the answer a SLAVE segment publishes, from the two facts its own engine
 * witnesses and nothing else.
 *
 *   `heard`     — reac_segment_heard's latch: is a master downstream arriving?
 *   `master_mac48` — the master MAC the FSM learned, packed big-endian into the
 *                 low 48 bits (reac_slave's published atomic copy); 0 = none
 *                 learned, which is a fact and reads as "none".
 *   `rate_hz`   — the pace this segment is locked to; <=0 publishes "0", which a
 *                 consumer reads as "no rate published" rather than as a guess.
 *
 * WHY EACH VALUE IS WHAT IT IS:
 *   master_state — heard means an OTHER master drives this wire and it is not
 *                  us, which is precisely REAC_SEGMENT_FOREIGN. Unheard is
 *                  REAC_SEGMENT_NONE: no evidence, not an assumption.
 *   rival_kind   — the slave RX gate passes the 40-channel master downstream and
 *                  nothing else, so a decoded frame is a REAC_MAX_CHANNELS
 *                  geometry; reac_rival_kind_from_channels turns that into
 *                  `desk` under the SAME law the master's arbitration uses. This
 *                  matters beyond tidiness: the console's role policy joins a
 *                  desk and refuses anything else, so an enrolled recorder that
 *                  published `none` here would be reported as refusing the very
 *                  master it is happily joined to.
 *   refusal      — reac_rival_refusal of that kind, which is "none" for a desk
 *                  and for no rival alike. Computed, never asserted.
 *   conflict     — always "0", and definitionally so: the flag means a foreign
 *                  master is live WHILE WE ARE MASTERING. A slave is not
 *                  mastering, so the dispute it reports cannot exist here.
 *   pace_source  — heard means the desk times the wire
 *                  (REAC_PACE_FOREIGN_MASTER). Unheard, nothing is timing it and
 *                  the honest answer is the reported fallback, never silence.
 */
void reac_segment_answer_slave(struct reac_segment_answer *out, int heard,
                               uint64_t master_mac48, int rate_hz);

#endif /* REAC_SEGMENT_IDENT_H */
