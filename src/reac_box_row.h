// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_box_row — the row a segment's nodes are sized and named from.
 *
 * A declaration a matrix row matches byte for byte is that row. A declaration no row
 * matches is a row built from the widths the box declared (its config-announce cells,
 * reac_ports_parse): reac-capture is its inputs, reac-playback its outputs, and it is
 * named after them. A Roland row with the same two widths lends its name; any other
 * combination is "S-4000S-<in><out>" — a stock S-4000S-3208 re-fitted to 16 in / 24
 * out (box 00:40:ab:c4:08:bc, 2026-10-06) is S-4000S-1624. No declaration, no row.
 *
 * The same declaration returns the same pointer, so a caller can still compare rows by
 * pointer; a new declaration returns the other of two slots. MAIN LOOP only. */
#ifndef REACPW_BOX_ROW_H
#define REACPW_BOX_ROW_H

#include <reac/reac_ctrlblk.h>

struct reac_box_row_slot {
	struct reac_box_model row;
	char token[24];
	char display[48];
};

struct reac_box_rows {
	struct reac_box_row_slot slot[2];
	int cur;     /* the slot last returned; -1 = none built yet */
};

void reac_box_rows_init(struct reac_box_rows *r);

/* `matched` when a row matched; else a row built from the declared widths; else NULL
 * (nothing declared, or a width that is not a box width). */
const struct reac_box_model *reac_box_row_resolve(struct reac_box_rows *r,
                                                  const struct reac_box_model *matched,
                                                  int declared_in, int declared_out);

#endif /* REACPW_BOX_ROW_H */
