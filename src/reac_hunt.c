// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_hunt — see reac_hunt.h for what this decides and which ruling each branch is.

#include "reac_hunt.h"

#include <reac/reac.h>   /* REAC_MAX_CHANNELS — the master downstream width */

#include <string.h>

const char *reac_hunt_verdict_name(enum reac_hunt_verdict v)
{
	switch (v) {
	case REAC_HUNT_MASTER:  return "master";
	case REAC_HUNT_SLAVE:   return "slave";
	case REAC_HUNT_REFUSED: return "refused";
	case REAC_HUNT_HUNTING:
	default:                return "hunting";
	}
}

void reac_hunt_init(struct reac_hunt *h, const uint8_t our_mac[6], uint64_t now_ns)
{
	memset(h, 0, sizeof *h);
	reac_disco_table_init(&h->table);
	reac_disco_peer_lock_init(&h->lock);
	if (our_mac)
		memcpy(h->our_mac, our_mac, 6);
	h->opened_ns = now_ns;
	h->verdict = REAC_HUNT_HUNTING;
}

void reac_hunt_pin(struct reac_hunt *h, enum reac_role role)
{
	h->pinned = 1;
	h->pin = role;
}

int reac_hunt_observe(struct reac_hunt *h, const uint8_t *frame, size_t len,
                      uint64_t now_ns, struct reac_disco_sighting *out)
{
	struct reac_disco_sighting s;
	if (reac_disco_classify_on_segment(&h->lock, frame, len, h->our_mac, &s) != 0)
		return -1;
	if (out)
		*out = s;
	/* THE WINDOW RUNS FROM THE FIRST SIGHTING, not from the socket opening. A sniffer
	 * can sit on a quiet office NIC for a week; when a segment finally powers up, the
	 * three cadences have to be spent HEARING it, or the first box to speak would take a
	 * wire whose desk is still booting. Everything on a segment comes up together, so
	 * this is the anchor that gives a slow desk its announce before we drive. */
	if (h->table.n == 0)
		h->opened_ns = now_ns;
	/* `owned` is 0: nothing is established here — this is the wire BEFORE we decide
	 * whether to drive it. */
	return reac_disco_table_observe(&h->table, &s, 0, now_ns) ? 1 : 0;
}

/* Is `e` still live, by the same staleness bar the arbitration uses? A peer heard once
 * and gone is not a peer. */
static int live(const struct reac_disco_entry *e, uint64_t now_ns)
{
	return !(now_ns > e->last_seen_ns && now_ns - e->last_seen_ns > REAC_DISCO_STALE_NS);
}

/* IS A STAGEBOX PRESENT ON THIS WIRE?
 *
 * Two kinds of evidence, and the second one is why this is not a one-line test:
 *
 *   - an unambiguous BOX sighting: a JOIN/box-ready/identity record, a box heartbeat, a
 *     box's own config announce, or a unicast FILLER feeding some master. This is the
 *     `a box JOIN/announce IS heard` case and it needs nothing else.
 *   - a BOX GEOMETRY from a peer whose role has not resolved. A box that has lost its
 *     master announces by FLOODING BROADCAST FILLER at wire rate on PHY-up
 *     (reac_fsm.h, byte-verified 2026-07-11), and a broadcast FILLER is deliberately
 *     classified UNKNOWN because a master's downstream audio is byte-identical in kind.
 *     Its WIDTH is not ambiguous: 40 channels is the master downstream and nothing
 *     else, every smaller legal geometry is a box (arbitration §2b, the same law
 *     reac_rival_kind_from_channels applies to a rival). A 16- or 32-channel flood is a
 *     stagebox standing on the wire with its hand up.
 *
 * Refusing the second kind would be the founding bug of this whole area: two boxes sat
 * ungranted on 2026-09-08 while the daemon hunted, because nothing turned "a box is
 * plainly there" into "so take the wire".
 */
static int box_present(const struct reac_disco_table *t, uint64_t now_ns)
{
	for (int i = 0; i < t->n; i++) {
		const struct reac_disco_entry *e = &t->e[i];
		if (!live(e, now_ns))
			continue;
		if (e->role == REAC_DISCO_ROLE_BOX)
			return 1;
		if (e->role == REAC_DISCO_ROLE_UNKNOWN &&
		    reac_rival_kind_from_channels(e->channels) == REAC_RIVAL_BOX)
			return 1;
	}
	return 0;
}

/* Is a 40-channel stream live on this wire from a peer we have NOT yet resolved to a
 * master? That is a desk's downstream audio with its announce not yet heard (or lost),
 * and it is the one case where the window must NOT expire into "vacant". Taking a wire
 * that is carrying a master downstream is the two-masters fault the seglock exists to
 * make impossible between processes; it is no better between a desk and us. */
static int desk_geometry_live(const struct reac_disco_table *t, const uint8_t our_mac[6],
                              uint64_t now_ns)
{
	for (int i = 0; i < t->n; i++) {
		const struct reac_disco_entry *e = &t->e[i];
		if (!live(e, now_ns))
			continue;
		if (our_mac && memcmp(e->mac, our_mac, 6) == 0)
			continue;
		if (reac_rival_kind_from_channels(e->channels) == REAC_RIVAL_DESK)
			return 1;
	}
	return 0;
}

static enum reac_hunt_verdict decide(const struct reac_hunt *h, uint64_t now_ns)
{
	/* A PIN IS AN ANSWER ABOUT THIS WIRE AND OUTRANKS THE HUNT. It is not an opinion the
	 * election weighs: `REAC_ROLE_<segment>` says which end of the pairing the operator
	 * wants here, and the hunt exists only for the segments nobody answered for. So the
	 * ONLY thing still waited on is the wire being a REAC segment at all — no window, no
	 * box evidence, no rival classification. A pinned segment that had to wait three
	 * seconds and find a box would be a setting the daemon second-guesses; worse, a
	 * pinned segment the hunt could not decide would never be served at all. */
	if (h->pinned)
		return h->table.n == 0 ? REAC_HUNT_HUNTING
		     : (h->pin == REAC_ROLE_SLAVE ? REAC_HUNT_SLAVE : REAC_HUNT_MASTER);

	/* A foreign master is unambiguous evidence, and §2b says WHAT it is decides what
	 * we do about it: a desk is joined, a stagebox on M and an unreadable rival are
	 * refused. Immediate — a desk on the wire is not a maybe, and there is nothing a
	 * longer wait could add. */
	if (h->arb.state == REAC_SEGMENT_FOREIGN)
		return h->arb.rival == REAC_RIVAL_DESK ? REAC_HUNT_SLAVE : REAC_HUNT_REFUSED;

	/* No master evidence. Before calling the wire vacant, refuse to race a 40-channel
	 * stream whose owner has not announced yet. */
	if (desk_geometry_live(&h->table, h->our_mac, now_ns))
		return REAC_HUNT_HUNTING;

	if (now_ns - h->opened_ns < REAC_HUNT_WINDOW_NS)
		return REAC_HUNT_HUNTING;

	/* The window closed on a silent wire with a box on it: §7 step 4's first half —
	 * we drive, probe, grant, establish. A wire with nothing on it at all is NOT taken:
	 * a segment is a place where REAC gear was heard, and driving needs evidence
	 * (§7's observe-then-act gate). */
	return box_present(&h->table, now_ns) ? REAC_HUNT_MASTER : REAC_HUNT_HUNTING;
}

int reac_hunt_step(struct reac_hunt *h, uint64_t now_ns)
{
	/* Age first: nothing here latches. A desk unplugged stops mastering the segment, a
	 * box taken out of M stops being refused, and the next step says so on its own. */
	reac_disco_table_age(&h->table, now_ns);
	/* Our own FSM is IDLE by construction — this runs BEFORE any listener opens, so we
	 * are neither probing nor established — and the pace is ours-and-undisciplined
	 * until something is decided. Both are facts, not placeholders. */
	reac_arbitrate(&h->table, h->our_mac, REAC_M_IDLE, REAC_PACE_FREE_RUN, now_ns, &h->arb);

	enum reac_hunt_verdict v = decide(h, now_ns);
	int changed = (v != h->verdict);
	h->verdict = v;
	return changed;
}

enum reac_role reac_hunt_role(const struct reac_hunt *h)
{
	return h->verdict == REAC_HUNT_SLAVE ? REAC_ROLE_SLAVE : REAC_ROLE_MASTER;
}

int reac_hunt_heard_anything(const struct reac_hunt *h)
{
	return h->table.n > 0;
}
