// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_boxreg — the multi-box registry.
 *
 * A master can drive several stageboxes on one REAC segment. Each box owns a
 * contiguous slice of the 40-slot fabric: its `nch` inputs land at slots
 * [base, base+nch). This registry maps a box's L2 MAC -> (base, nch, name) so
 * the RX can place each box's decoded audio at its fabric slots and the UI can
 * label them. Allocation is by first-seen (JOIN) order: the next box takes the
 * lowest free contiguous range wide enough for it. Boxes may also be
 * pre-declared (CLI) with a fixed base and/or a friendly name.
 *
 * Pure data + logic — no threads, no PipeWire. Unit-tested in isolation.
 */
#ifndef REAC_BOXREG_H
#define REAC_BOXREG_H

#include <stdint.h>

#define REAC_BOXREG_MAX_BOXES 5      /* 5 x 8ch = 40 = a full fabric of S-0808s */
#define REAC_BOXREG_NAME_MAX  32
#define REAC_BOXREG_FABRIC     40    /* the REAC fabric slot count */

struct reac_box {
	uint8_t  mac[6];                     /* L2 source; all-zero = a pre-declared slot */
	int      base;                       /* first fabric slot (0..39) */
	int      nch;                        /* box input width (even, 2..40) */
	int      established;                 /* handshake complete (RX/FSM sets this) */
	int      pinned;                     /* base was pre-declared, not auto-allocated */
	char     name[REAC_BOXREG_NAME_MAX]; /* operator label; "" = fall back to model */
};

struct reac_boxreg {
	struct reac_box box[REAC_BOXREG_MAX_BOXES];
	int n;          /* boxes registered */
	int fabric;     /* total fabric slots (<= REAC_BOXREG_FABRIC) */
};

/* Reset the registry to empty with `fabric` total slots (0 or >FABRIC -> 40). */
void reac_boxreg_init(struct reac_boxreg *r, int fabric);

/* Index of the box with this MAC, or -1. */
int reac_boxreg_find(const struct reac_boxreg *r, const uint8_t mac[6]);

/* Pre-declare a box (CLI) BEFORE it joins: reserve `nch` slots (even) with an
 * optional `name` (NULL/"" = none) at `base` (>=0 pins it; <0 = auto-allocate the
 * lowest free range). MAC is left zero until a matching-width box actually joins.
 * Returns the box index, or -1 if invalid width / no room / base collision. */
int reac_boxreg_declare(struct reac_boxreg *r, int nch, const char *name, int base);

/* Register a JOINed box by (mac, nch). If a pre-declared, still-unbound slot of
 * the same width exists, bind this MAC to it (honouring its base + name);
 * otherwise auto-allocate the lowest free contiguous range of `nch` slots.
 * Idempotent: a MAC already present returns its existing index. Returns the box
 * index, or -1 if the width is invalid or the fabric is full. */
int reac_boxreg_add(struct reac_boxreg *r, const uint8_t mac[6], int nch);

/* Set / overwrite a box's operator name. */
void reac_boxreg_set_name(struct reac_boxreg *r, int idx, const char *name);

#endif /* REAC_BOXREG_H */
