// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_hunt — WHICH END OF THE PAIRING WE TAKE ON A WIRE NOBODY CONFIGURED.
 *
 * This is step 4 of the autodetect chain, verbatim, for a segment that has just been
 * heard (openmixer docs/design/specs/2026-08-23-reac-trunk-vlan-daemon.md §7):
 *
 *   "no foreign master -> we drive, probe, grant, establish; a foreign master -> we
 *    join as a slave and follow its pace (arbitration spec §2)"
 *
 * and it is the daemon half of arbitration §8b's `auto`, which is the product default:
 * "no master on the wire, we master it; a DESK masters it, we slave to it; a STAGEBOX
 * masters it, we refuse and say so, because a box in the wrong switch position is a
 * mistake to report, not a topology to obey."
 *
 * IT DECIDES NOTHING BY ITSELF THAT reac_arbitration HAS NOT ALREADY OBSERVED.
 * reac_arbitration stays passive — it publishes who drives the segment — and this
 * module is the thin ACT half over it: accumulate the sightings of one wire in the
 * discovery table the rest of the daemon already uses, ask reac_arbitrate what the
 * aggregate is, and turn that into the role this segment launches in. Everything about
 * classification (a rival is separated from a desk by its frame GEOMETRY, never by a
 * role byte — arbitration §2b) is reused, not restated.
 *
 * THE TWO AXES STAY TWO AXES (arbitration §8). `enum reac_role` is the WIRE vocabulary
 * — which end of the desk<->stagebox pairing we present as — and has no `auto`. The
 * INTENT (`master` | `slave` | `auto`) lives in reac_role.h beside it, and `auto` is
 * what a segment with nothing configured means. This module resolves the second into
 * the first; the clock axis is untouched by any of it.
 *
 * WHY IT REFUSES RATHER THAN FIGHTS. REAC keeps ONE master per segment. A stagebox
 * whose mode switch is on M masters the wire while emitting its own (box) width, and
 * slave-joining it would present this console as a box to a box, hide a whole box of
 * channels and obey a misconfiguration instead of naming it. So it is logged with the
 * remedy and left alone — never probed at, never out-shouted.
 *
 * Main-thread only, like the discovery table it holds: one hunt per passive sniffer,
 * alive only until its segment is served (or refused). It transmits nothing; hearing is
 * the gate to serve and this is what reads the hearing.
 */
#ifndef REAC_HUNT_H
#define REAC_HUNT_H

#include "reac_arbitration.h"
#include "reac_disco.h"
#include "reac_role.h"

#include <stdint.h>
#include <stddef.h>

/* HOW LONG A VACANT WIRE IS WATCHED BEFORE WE TAKE IT — three master announce
 * cadences.
 *
 * The number is read off the protocol this daemon already emits, not picked: a master
 * announces itself with a cfea once per second and fills the gaps with FILLER
 * (`reac_master.c`, `announce_tick >= m->fps`, one frame per second at every rate; the
 * master heartbeat runs at the same ~1/s). One cadence would call a desk absent on a
 * single lost announce; two on two in a row; THREE is the first window that survives
 * two consecutive losses, and it still sits comfortably inside the 5 s
 * REAC_DISCO_STALE_NS bar this codebase already uses for "a device is really gone".
 *
 * The window bounds only the SILENT case. A desk that IS there is normally heard on its
 * first announce and joined then — the wait is what a wire with nothing on it costs,
 * once, and it is the difference between hunting for three seconds and hunting forever.
 */
#define REAC_HUNT_WINDOW_NS (3ULL * 1000000000ULL)

/* What the hunt has concluded about this segment. `HUNTING` is not a failure: it is the
 * honest state of a wire that has not answered yet, and it is REPORTED rather than spun
 * on (trunk-VLAN spec §9 — a can't-act state is a coded refusal, never a silent
 * spinner). */
enum reac_hunt_verdict {
	REAC_HUNT_HUNTING = 0,   /* nothing decides it yet; keep listening */
	REAC_HUNT_MASTER,        /* no master heard and a box is present: we drive and grant */
	REAC_HUNT_SLAVE,         /* a desk masters this wire: join it and follow its pace */
	REAC_HUNT_REFUSED,       /* a stagebox (or an unreadable rival) masters it: say so */
};

const char *reac_hunt_verdict_name(enum reac_hunt_verdict v);

struct reac_hunt {
	/* The evidence, in the same table the established path publishes from — so the
	 * hunt's view of a wire and the segment's view of it are the same code. */
	struct reac_disco_table table;
	/* REAC is physically point-to-point, so a FILLER frame (checksum-exempt) is only
	 * trusted from the peer a real control frame already proved. Same defence the
	 * pacer's own classifier runs; the hunt is a segment too. */
	struct reac_disco_peer_lock lock;
	uint8_t our_mac[6];
	/* When the window started: the hunt's own opening, re-anchored to the FIRST sighting
	 * (reac_hunt_observe explains why). Never moved again — a second peer does not buy
	 * the wire another three seconds. */
	uint64_t opened_ns;
	enum reac_hunt_verdict verdict;
	/* The aggregate the current verdict was read from — the rival's MAC and kind for
	 * the refusal sentence, never re-derived by the caller. */
	struct reac_arbitration arb;
};

/* Start hunting on a segment. `our_mac` is this NIC's own address: our echo is not
 * evidence of anybody else. */
void reac_hunt_init(struct reac_hunt *h, const uint8_t our_mac[6], uint64_t now_ns);

/* Offer one raw frame. Returns 1 when it was a sighting that changed the table
 * OBSERVABLY (a new peer, a sharper role or model) — which is what deserves a log line;
 * 0 when it was a sighting that only refreshed liveness; -1 when the frame is not
 * evidence of REAC gear at all. `*out` is filled on any sighting (return >= 0). */
int reac_hunt_observe(struct reac_hunt *h, const uint8_t *frame, size_t len,
                      uint64_t now_ns, struct reac_disco_sighting *out);

/* Re-decide. Ages the table first, so an unplugged desk stops mastering the segment and
 * a box switched out of M stops being refused, without anything latching. Returns 1 when
 * the verdict CHANGED (log it), 0 when it stands. */
int reac_hunt_step(struct reac_hunt *h, uint64_t now_ns);

/* The wire role the verdict resolves to. MASTER for a vacant wire, SLAVE for a desk;
 * meaningless (and never acted on) while HUNTING or REFUSED, where it answers MASTER
 * only because `enum reac_role` has no third value — the caller gates on the verdict. */
enum reac_role reac_hunt_role(const struct reac_hunt *h);

/* Has any REAC gear been heard on this wire at all? Distinguishes "quiet" from
 * "undecided" for the report (trunk-VLAN §9: could-not-scan must never render as
 * found-nothing). */
int reac_hunt_heard_anything(const struct reac_hunt *h);

#endif /* REAC_HUNT_H */
