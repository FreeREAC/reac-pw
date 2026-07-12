// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_boxreg.h"
#include <string.h>
#include <stdio.h>

void reac_boxreg_init(struct reac_boxreg *r, int fabric)
{
	memset(r, 0, sizeof *r);
	r->fabric = (fabric > 0 && fabric <= REAC_BOXREG_FABRIC) ? fabric : REAC_BOXREG_FABRIC;
}

static int mac_is_zero(const uint8_t mac[6])
{
	for (int k = 0; k < 6; k++)
		if (mac[k])
			return 0;
	return 1;
}

int reac_boxreg_find(const struct reac_boxreg *r, const uint8_t mac[6])
{
	if (mac_is_zero(mac))
		return -1;   /* the zero MAC marks an unbound pre-declared slot, not a box */
	for (int i = 0; i < r->n; i++)
		if (memcmp(r->box[i].mac, mac, 6) == 0)
			return i;
	return -1;
}

/* Does [base, base+nch) overlap any registered box's slot range? */
static int range_taken(const struct reac_boxreg *r, int base, int nch)
{
	for (int i = 0; i < r->n; i++) {
		int b = r->box[i].base, e = b + r->box[i].nch;
		if (base < e && b < base + nch)
			return 1;
	}
	return 0;
}

/* Lowest base whose [base, base+nch) is free and fits the fabric, or -1. Fills
 * gaps left by a departed box, so the allocation stays compact. */
static int lowest_free_base(const struct reac_boxreg *r, int nch)
{
	for (int base = 0; base + nch <= r->fabric; base++)
		if (!range_taken(r, base, nch))
			return base;
	return -1;
}

static int width_ok(const struct reac_boxreg *r, int nch)
{
	return nch >= 2 && nch <= r->fabric && (nch & 1) == 0;
}

int reac_boxreg_declare(struct reac_boxreg *r, int nch, const char *name, int base)
{
	if (!width_ok(r, nch) || r->n >= REAC_BOXREG_MAX_BOXES)
		return -1;
	int pinned = base >= 0;
	if (pinned) {
		if (base + nch > r->fabric || range_taken(r, base, nch))
			return -1;
	} else {
		base = lowest_free_base(r, nch);
		if (base < 0)
			return -1;
	}
	struct reac_box *b = &r->box[r->n];
	memset(b, 0, sizeof *b);       /* MAC stays zero: unbound until a box JOINs */
	b->base = base;
	b->nch = nch;
	b->pinned = pinned;
	if (name)
		snprintf(b->name, sizeof b->name, "%s", name);
	return r->n++;
}

int reac_boxreg_add(struct reac_boxreg *r, const uint8_t mac[6], int nch)
{
	if (!width_ok(r, nch))
		return -1;
	int idx = reac_boxreg_find(r, mac);
	if (idx >= 0)
		return idx;   /* already registered — idempotent */

	/* Bind to a pre-declared, still-unbound slot of the SAME width (honour its
	 * pinned base + operator name). First-declared wins. */
	for (int i = 0; i < r->n; i++) {
		if (mac_is_zero(r->box[i].mac) && r->box[i].nch == nch) {
			memcpy(r->box[i].mac, mac, 6);
			return i;
		}
	}

	/* Otherwise auto-allocate a fresh box at the lowest free contiguous range. */
	if (r->n >= REAC_BOXREG_MAX_BOXES)
		return -1;
	int base = lowest_free_base(r, nch);
	if (base < 0)
		return -1;
	struct reac_box *b = &r->box[r->n];
	memset(b, 0, sizeof *b);
	memcpy(b->mac, mac, 6);
	b->base = base;
	b->nch = nch;
	return r->n++;
}

void reac_boxreg_set_name(struct reac_boxreg *r, int idx, const char *name)
{
	if (idx < 0 || idx >= r->n)
		return;
	snprintf(r->box[idx].name, sizeof r->box[idx].name, "%s", name ? name : "");
}
