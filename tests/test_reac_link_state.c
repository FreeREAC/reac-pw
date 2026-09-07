// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test for reac_link_state_from_master / reac_link_state_name — the pure
 * mapping the reac.link-state PipeWire property (task #154's stagebox-badge
 * need) is stamped from — and for reac_box_mac_str, the reac.box.mac half of the
 * same badge set. Pins:
 *   (a) every reac_master_state maps to its badge string, with IDLE folded
 *       into "probing" alongside PROBING itself (#130: a real master never
 *       observably idles);
 *   (b) the just_dropped overlay always wins, regardless of `st`;
 *   (c) the exact wire strings a consumer (openmixer's stagebox card) matches
 *       on are stable ("probing" / "granting" / "established" / "dropped");
 *   (d) reac.box.mac carries THE BOX'S OWN address, latched from the source MAC
 *       of the JOIN the master granted, in the lowercase colon form the master's
 *       log prints — and reads "none" before the JOIN and again once the box is
 *       forgotten. (d) drives the real reac_master rather than hand-feeding the
 *       formatter, because the defect it exists for was a consumer keying a box
 *       registry on OUR NIC's address: a formatter test could not have seen it,
 *       and the fact under test is that the string comes from the PEER.
 */
#include "reac_link_state.h"
#include "reac_mac.h"
#include "reac_master.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The byte-verified zoneA-48k S-1608 cold-connect block a real box sends as its
 * JOIN (the same constant tests/test_reac_master.c drives the handshake with). */
static const uint8_t ZONEA_JOIN[32] = {
 0x04,0x03,0x00,0x14,0x00,0x02,0x00,0xfe,0x0f,0xf0,0x41,0x0a,0x00,0x00,0x12,0x12,
 0x01,0x00,0x06,0x00,0x01,0x00,0x78,0xf7,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
/* OUR NIC and THE BOX, deliberately different vendors' addresses: this test is
 * worthless if the two can be confused. The box address is the one the live rig's
 * master log prints ("box JOIN seen (cdea 04 03, unicast from 00:40:ab:c4:80:3b)"). */
static const uint8_t OUR_NIC[6] = { 0x00, 0x14, 0x5c, 0x9b, 0x28, 0x2d };
static const uint8_t BOX[6]     = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x3b };

/* The master holds any forward edge until a scene transfer completes, so a JOIN
 * only opens GRANTING from the quiet window between two pushes — the same stand
 * a real box takes. */
static void deliver_scene(struct reac_master *m)
{
	uint16_t c;
	int ix;
	long guard = 0;
	while ((m->scene_complete == 0 || m->scene_inflight) &&
	       guard++ < 4L * m->cycle_len)
		(void)reac_master_next(m, &c, &ix);
}

/* What sink_publish_link_props stamps into reac.box.mac, composed exactly as it
 * composes it: the master's latched address, packed for the cross-thread hop,
 * formatted by the shared formatter. */
static void badge_box_mac(const struct reac_master *m, char *out, size_t cap)
{
	reac_box_mac_str(reac_mac48_pack(m->box_mac), out, cap);
}

#define CHK(cond) do { if (!(cond)) { \
	fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__); return 1; } } while (0)

int main(void)
{
	/* ---- (a) each FSM state maps to its badge value, not dropped. */
	CHK(reac_link_state_from_master(REAC_M_IDLE, 0) == REAC_LINK_PROBING);
	CHK(reac_link_state_from_master(REAC_M_PROBING, 0) == REAC_LINK_PROBING);
	CHK(reac_link_state_from_master(REAC_M_GRANTING, 0) == REAC_LINK_GRANTING);
	CHK(reac_link_state_from_master(REAC_M_ESTABLISHED, 0) == REAC_LINK_ESTABLISHED);

	/* ---- (b) the drop overlay wins over every state, including a state that
	 * would otherwise read as good link (ESTABLISHED/GRANTING) — the caller
	 * only ever sets it true on the exact drain cycle the backward transition
	 * fired, but the mapping itself must not second-guess that. */
	CHK(reac_link_state_from_master(REAC_M_IDLE, 1) == REAC_LINK_DROPPED);
	CHK(reac_link_state_from_master(REAC_M_PROBING, 1) == REAC_LINK_DROPPED);
	CHK(reac_link_state_from_master(REAC_M_GRANTING, 1) == REAC_LINK_DROPPED);
	CHK(reac_link_state_from_master(REAC_M_ESTABLISHED, 1) == REAC_LINK_DROPPED);

	/* ---- (c) the exact wire strings the badge consumer matches on. */
	CHK(strcmp(reac_link_state_name(REAC_LINK_PROBING), "probing") == 0);
	CHK(strcmp(reac_link_state_name(REAC_LINK_GRANTING), "granting") == 0);
	CHK(strcmp(reac_link_state_name(REAC_LINK_ESTABLISHED), "established") == 0);
	CHK(strcmp(reac_link_state_name(REAC_LINK_DROPPED), "dropped") == 0);

	/* ---- (d) reac.box.mac is the BOX's address, not ours.
	 *
	 * The formatter first, including the sentinel: 0 is reac_mac48_pack's "no
	 * MAC" (no device carries the all-zero address) and must read as "none", so
	 * that a consumer can never parse an absent box into 00:00:00:00:00:00. */
	char mac[REAC_BOX_MAC_STR_CAP];
	reac_box_mac_str(0, mac, sizeof mac);
	CHK(strcmp(mac, REAC_BOX_MAC_NONE) == 0);
	reac_box_mac_str(reac_mac48_pack(BOX), mac, sizeof mac);
	CHK(strcmp(mac, "00:40:ab:c4:80:3b") == 0);   /* lowercase, colon-separated */

	/* Then the fact that matters, through the master: a fresh master has no box,
	 * so the badge says so even though it already knows OUR address. */
	struct reac_master m;
	reac_master_init(&m, OUR_NIC, NULL, 8000);
	badge_box_mac(&m, mac, sizeof mac);
	CHK(strcmp(mac, REAC_BOX_MAC_NONE) == 0);

	/* The JOIN latches the peer. THE BADGE MUST NOT BE OUR NIC — that is the
	 * whole defect: openmixer keyed its `reac:<box mac>` patch registry on
	 * reac.master.mac, which in the master role is this machine, and missed on
	 * every rig (openmixer 2026-09-06-rig-headamp-and-clip-findings.md §5). */
	deliver_scene(&m);
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) == 1);
	CHK(m.state == REAC_M_GRANTING);
	badge_box_mac(&m, mac, sizeof mac);
	CHK(strcmp(mac, "00:40:ab:c4:80:3b") == 0);
	char ours[REAC_BOX_MAC_STR_CAP];
	reac_box_mac_str(reac_mac48_pack(OUR_NIC), ours, sizeof ours);
	CHK(strcmp(mac, ours) != 0);

	/* A second box on the same segment moves the badge with it: the address is a
	 * fact about the peer we are courting NOW, never the first one ever seen. */
	static const uint8_t BOX2[6] = { 0x00, 0x40, 0xab, 0x09, 0x09, 0x09 };
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX2, ZONEA_JOIN) == 1);
	badge_box_mac(&m, mac, sizeof mac);
	CHK(strcmp(mac, "00:40:ab:09:09:09") == 0);

	/* DEPARTURE CLEARS IT. reac_master_forget_box runs on every backward
	 * transition to PROBING, and the badge has to go with the box — a stale
	 * address names a chassis that has left the wire, which is worse than no
	 * name at all. */
	reac_master_forget_box(&m);
	badge_box_mac(&m, mac, sizeof mac);
	CHK(strcmp(mac, REAC_BOX_MAC_NONE) == 0);

	printf("OK: link-state mapping — every FSM state -> its badge string, "
	       "drop overlay wins, wire strings stable; reac.box.mac is the JOIN's "
	       "source MAC and clears on departure\n");
	return 0;
}
