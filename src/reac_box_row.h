// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_box_row — the box a segment's nodes are sized and named from, as its frames say.
 *
 * Operator ruling 2026-10-07 (FreeREAC/reac-pw#5): every fact about a connected box comes
 * from the protocol. The widths are its DECLARATION (config-announce cells,
 * reac_ports_parse); the name is libreac's reac_box_name over the family its identity
 * page's hw block names (or the name the box sent, the S-0808's) and those widths; the
 * firmware and hw block are stamped from the same page. The model catalogue never sizes
 * or names the box: the entry its declaration matched byte for byte is only COMPARED, and
 * a disagreement is logged as a catalogue defect.
 *
 * The name waits for the identity page, because relabelling a node rebuilds it: no row is
 * returned until the hw block has arrived or `wait_over` says the binding has waited long
 * enough, and then a box whose family nobody captured is named by its widths
 * (REAC-0816) and said so. The same facts return the same pointer, so a caller can
 * compare rows by pointer; new facts return the other of two slots. MAIN LOOP only. */
#ifndef REACPW_BOX_ROW_H
#define REACPW_BOX_ROW_H

#include <reac/reac_box_facts.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_identity.h>
#include <reac/reac_link_state.h>

#include <stdint.h>

struct reac_box_row_slot {
	struct reac_box_model row;
	char name[REAC_BOX_NAME_MAX];
	char token[REAC_BOX_NAME_TOKEN_MAX];
	char display[REAC_BOX_NAME_DISPLAY_MAX];
};

struct reac_box_rows {
	struct reac_box_row_slot slot[2];
	int cur;     /* the slot last returned; -1 = none built yet */
	/* What the last build found, for the caller and the tests: the catalogue-defect
	 * mask (reac_box_catalogue_defect) and whether the family was unknown. */
	int defect;
	int unknown_family;
};

void reac_box_rows_init(struct reac_box_rows *r);

/* The box as its frames say, or NULL: nothing declared, a width that is not a box
 * width, or the identity page not in yet and `wait_over` 0. `catalogue` is the entry the
 * declaration matched (or NULL) and is only compared; `id` may be NULL. A newly built row
 * logs, once, a catalogue defect or an unknown hw family to stderr. */
const struct reac_box_model *reac_box_row_resolve(struct reac_box_rows *r,
                                                  const struct reac_box_model *catalogue,
                                                  int declared_in, int declared_out,
                                                  const struct reac_identity *id,
                                                  int wait_over);

/* Does this row get a reac-playback node? Only when it declares outputs: a box that
 * declares 0 outputs (an S-4000S-4000, 40 in / 0 out) has none, rather than a node
 * the graph would fill with a stereo pair. */
int reac_box_row_has_playback(const struct reac_box_model *bm);

/* THE SEGMENT'S BOX BADGE, stamped through `set`: reac.link-state (left alone when
 * `link_state` is NULL), reac.box-model (the token), reac.box-name, reac.box-width,
 * reac.box.mac and the identity page's reac.box-firmware / reac.box.reac_version /
 * reac.box-hw. Both nodes of a segment stamp through this one function. A NULL row is
 * "none" / "" / "0x0"; a 0 MAC and an empty identity are stamped too, so a departed
 * box's values never stay behind. */
void reac_box_row_badge_publish(const struct reac_box_model *bm, const char *link_state,
                                uint64_t box_mac48, const struct reac_identity *id,
                                reac_prop_set_fn set, void *ctx);

#endif /* REACPW_BOX_ROW_H */
