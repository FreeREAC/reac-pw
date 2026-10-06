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
#include <reac/reac_identity.h>
#include <reac/reac_link_state.h>

#include <stdint.h>

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

/* Does this row get a reac-playback node? Only when it declares outputs: a box that
 * declares 0 outputs (an S-4000S-4000, 40 in / 0 out) has none, rather than a node
 * the graph would fill with a stereo pair. */
int reac_box_row_has_playback(const struct reac_box_model *bm);

/* THE SEGMENT'S BOX BADGE, stamped through `set`: reac.link-state (left alone when
 * `link_state` is NULL), reac.box-model, reac.box-width, reac.box.mac and the identity
 * page's reac.box-firmware / reac.box.reac_version / reac.box-hw. Both nodes of a
 * segment stamp through this one function, so a box that has only a reac-capture (an
 * S-4000S-4000 declares no outputs) carries the same identity a box with both nodes
 * carries on its reac-playback. A NULL row is "none" / "0x0"; a 0 MAC and an empty
 * identity are stamped too, so a departed box's values never stay behind. */
void reac_box_row_badge_publish(const struct reac_box_model *bm, const char *link_state,
                                uint64_t box_mac48, const struct reac_identity *id,
                                reac_prop_set_fn set, void *ctx);

#endif /* REACPW_BOX_ROW_H */
