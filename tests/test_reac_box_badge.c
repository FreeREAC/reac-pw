// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac.box.mac — THE WHOLE SEAM, from a cold-connect frame on the wire to the
 * key/value pair that lands on the node's properties.
 *
 * Why this is one test and not two. The defect it exists for was not a bad
 * formatter: it was that the badge set named the box five ways (model, width,
 * firmware, hw block, source) and never by its ADDRESS, so openmixer's segment
 * row — whose stagebox registry is keyed `reac:<box mac>` — looked the patch name
 * up by reac.master.mac, which in the master role is OUR NIC, and read
 * patchName "" on every rig (openmixer
 * docs/design/notes/2026-09-06-rig-headamp-and-clip-findings.md §5). A formatter
 * test cannot see any of that. What has to be proven is that the RIGHT KEY gets
 * the PEER'S ADDRESS, and gets it again as "none" the moment the box leaves.
 *
 * So the composition and the stamp are one function, reac_box_mac_publish, and
 * both nodes go through it: the sink hands it pw_properties_set over the props it
 * is about to update, the reac-capture mirror hands it its own, and this test
 * hands it a recording fake. pw_properties lives behind libpipewire and no unit
 * test here links it, which is exactly why the seam had no proof before.
 *
 * NO SOCKET, SO IT NEVER SKIPS. The pacer is built by hand with fd = -1 (the
 * construction reac_pacer_rx_ingest is documented for) and fed real frames built
 * by reac_ctrl's builders, so the answer comes out of the same code path the
 * daemon runs — no CAP_NET_RAW, no wire, no PipeWire. */
#include "reac_link_state.h"
#include "reac_mac.h"
#include "reac_pacer.h"
#include "reac_ctrl.h"

#include <reac/reac.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define CHK(c) do { if (!(c)) { \
	fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* THE FAKE PROPS DICT: what pw_properties would hold, with the same MERGE
 * semantics (a key written twice keeps the last value), plus a write counter so a
 * test can tell "stamped none" from "not stamped at all" — the distinction the
 * whole update_properties-merges argument turns on. */
#define FAKE_MAX 8
struct fake_props {
	char key[FAKE_MAX][48];
	char val[FAKE_MAX][48];
	int  n;       /* distinct keys held */
	int  writes;  /* set() calls, including overwrites */
};

static void fake_set(void *ctx, const char *key, const char *value)
{
	struct fake_props *f = ctx;
	f->writes++;
	for (int i = 0; i < f->n; i++) {
		if (strcmp(f->key[i], key) == 0) {
			snprintf(f->val[i], sizeof f->val[i], "%s", value);
			return;
		}
	}
	if (f->n >= FAKE_MAX)
		return;
	snprintf(f->key[f->n], sizeof f->key[f->n], "%s", key);
	snprintf(f->val[f->n], sizeof f->val[f->n], "%s", value);
	f->n++;
}

/* The value held at `key`, or NULL when the key was never written. */
static const char *fake_get(const struct fake_props *f, const char *key)
{
	for (int i = 0; i < f->n; i++)
		if (strcmp(f->key[i], key) == 0)
			return f->val[i];
	return NULL;
}

/* The byte-verified zoneA-48k S-1608 cold-connect the master answers with a
 * grant, and the two addresses this test exists to keep apart. The box address is
 * the one the live rig's master log prints: "box JOIN seen (cdea 04 03, unicast
 * from 00:40:ab:c4:80:3b)". */
static const uint8_t OUR[6]  = { 0x00, 0x14, 0x5c, 0x9b, 0x28, 0x2d };  /* our NIC */
static const uint8_t BOX[6]  = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x3b };
static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* Stamp the badge the way both nodes stamp it: the pacer's cross-thread atomic
 * through the REAL composer, into the caller's dict. No formatting happens here —
 * that is the point. */
static void publish(struct reac_pacer *p, struct fake_props *f)
{
	reac_box_mac_publish(reac_pacer_box_mac48(p), fake_set, f);
}

int main(void)
{
	/* ---- 1. The stamp itself: the right KEY, and "none" is WRITTEN. */
	{
		struct fake_props f = { 0 };
		reac_box_mac_publish(0, fake_set, &f);
		CHK(f.writes == 1);            /* absence is stamped, never skipped */
		CHK(f.n == 1);                 /* and it stamps ONE key, not a set */
		CHK(fake_get(&f, REAC_PROP_BOX_MAC) != NULL);
		CHK(strcmp(fake_get(&f, REAC_PROP_BOX_MAC), REAC_BOX_MAC_NONE) == 0);
		/* The key is spelled out here, not taken from the macro: a consumer
		 * matches this literal, so a rename has to break a test rather than a
		 * rig. openmixer reads exactly "reac.box.mac". */
		CHK(strcmp(f.key[0], "reac.box.mac") == 0);

		/* A real address, lowercase and colon-separated — the form the master's
		 * own log prints, so the two can be compared by eye during a fault. */
		reac_box_mac_publish(reac_mac48_pack(BOX), fake_set, &f);
		CHK(f.writes == 2 && f.n == 1);   /* merged onto the same key */
		CHK(strcmp(fake_get(&f, REAC_PROP_BOX_MAC), "00:40:ab:c4:80:3b") == 0);

		/* And back to none: a departed box must not leave its address standing. */
		reac_box_mac_publish(0, fake_set, &f);
		CHK(strcmp(fake_get(&f, REAC_PROP_BOX_MAC), REAC_BOX_MAC_NONE) == 0);
	}

	/* ---- 2. THE SEAM, end to end: real frames -> reac_pacer_rx_ingest -> the
	 * cross-thread atomic -> the composer -> the key a consumer reads. */
	struct reac_pacer p;
	memset(&p, 0, sizeof p);
	p.fd = -1;
	p.fps = 8000;
	memcpy(p.src, OUR, 6);
	CHK(reac_frame_ring_init(&p.ring, 8, 2048) == 0);
	reac_master_init(&p.master, OUR, NULL, 8000);
	p.prev_state = REAC_M_IDLE;

	struct fake_props f = { 0 };
	uint8_t bf[2048];
	size_t bn;

	/* (a) A box FLOODING presence is not a box we have joined. Hearing a chassis
	 * and enrolling it are different facts, and the badge answers only the second
	 * — the segment's discovery list is where a mere sighting belongs. */
	bn = reac_ctrl_build_upstream_filler(bf, BCAST, BOX, 1, 16, NULL, 12);
	reac_pacer_rx_ingest(&p, bf, bn);
	publish(&p, &f);
	CHK(strcmp(fake_get(&f, REAC_PROP_BOX_MAC), REAC_BOX_MAC_NONE) == 0);

	/* (b) The cold-connect. Its forward edge is HELD until the scene push
	 * completes (reac_master.c), so drive the cadence to the end of a transfer
	 * the way a box that joins between two pushes stands. */
	bn = reac_ctrl_build_coldconnect(bf, OUR, BOX, 2, 16, NULL, 12);
	reac_pacer_rx_ingest(&p, bf, bn);
	{
		uint16_t c; int ix;
		long guard = 0;
		while ((p.master.scene_complete == 0 || p.master.scene_inflight) &&
		       guard++ < 4L * p.master.cycle_len)
			(void)reac_master_next(&p.master, &c, &ix);
		CHK(p.master.scene_complete == 1);
	}
	CHK(p.master.state == REAC_M_GRANTING);

	/* (c) The box declares itself; the next ingest publishes. THE VALUE IS THE
	 * PEER'S, and this is the assertion the defect would have failed: reading
	 * p.src here gives our own NIC, which is what a consumer keying a Roland
	 * registry matched against and missed. */
	bn = reac_ctrl_build_config_announce(bf, OUR, BOX, 3, 16);
	reac_pacer_rx_ingest(&p, bf, bn);
	publish(&p, &f);
	CHK(strcmp(fake_get(&f, REAC_PROP_BOX_MAC), "00:40:ab:c4:80:3b") == 0);
	{
		/* named explicitly, so "the badge is our NIC" cannot pass by resembling
		 * a MAC-shaped string */
		struct fake_props ours = { 0 };
		reac_box_mac_publish(reac_mac48_pack(OUR), fake_set, &ours);
		CHK(strcmp(fake_get(&f, REAC_PROP_BOX_MAC),
		           fake_get(&ours, REAC_PROP_BOX_MAC)) != 0);
	}

	/* (d) DEPARTURE CLEARS IT. The box's BYE (a heartbeat with selector 0x00)
	 * drops the master back to PROBING and reac_master_forget_box runs; the badge
	 * has to go with the box, because a stale address names a chassis that has
	 * left the wire — worse than no name, since the console would keep a patch
	 * label for equipment nobody can address. */
	bn = reac_ctrl_build_box_hb(bf, OUR, BOX, 4, 16);
	bf[22] = 0x00;                  /* selector 0x00 = the box's BYE */
	reac_ctrl_checksum_apply(bf);   /* a corrupt block is not a BYE */
	reac_pacer_rx_ingest(&p, bf, bn);
	CHK(p.master.state == REAC_M_PROBING);
	CHK(reac_master_has_box(&p.master) == 0);
	publish(&p, &f);
	CHK(strcmp(fake_get(&f, REAC_PROP_BOX_MAC), REAC_BOX_MAC_NONE) == 0);

	/* (e) A DIFFERENT box on the same segment takes the badge with it. The
	 * address is a fact about the peer we are courting now, never the first one
	 * ever seen — a swapped stagebox that inherited its predecessor's name is the
	 * same class of fault as a stale model or a stale head-amp base. */
	static const uint8_t BOX2[6] = { 0x00, 0x40, 0xab, 0x09, 0x09, 0x09 };
	bn = reac_ctrl_build_coldconnect(bf, OUR, BOX2, 5, 16, NULL, 12);
	reac_pacer_rx_ingest(&p, bf, bn);
	{
		uint16_t c; int ix;
		long guard = 0;
		while (p.master.state != REAC_M_GRANTING && guard++ < 4L * p.master.cycle_len)
			(void)reac_master_next(&p.master, &c, &ix);
	}
	bn = reac_ctrl_build_config_announce(bf, OUR, BOX2, 6, 16);
	reac_pacer_rx_ingest(&p, bf, bn);
	publish(&p, &f);
	CHK(strcmp(fake_get(&f, REAC_PROP_BOX_MAC), "00:40:ab:09:09:09") == 0);

	reac_frame_ring_free(&p.ring);

	printf("OK: reac.box.mac — the JOIN's source MAC reaches the node property, "
	       "clears on departure, follows a swap, and is never our own NIC\n");
	return 0;
}
