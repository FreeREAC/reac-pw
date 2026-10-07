// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The box a segment's nodes are sized and named from. See reac_box_row.h. */
#include "reac_box_row.h"

#include <stdio.h>
#include <string.h>

void reac_box_rows_init(struct reac_box_rows *r)
{
	if (!r)
		return;
	memset(r, 0, sizeof *r);
	r->cur = -1;
}

const struct reac_box_model *reac_box_row_resolve(struct reac_box_rows *r,
                                                  const struct reac_box_model *catalogue,
                                                  int declared_in, int declared_out,
                                                  const struct reac_identity *id,
                                                  int wait_over)
{
	if (!r || (declared_in == 0 && declared_out == 0))
		return NULL;
	const int have_hw = id && id->has_reac_version;
	if (!have_hw && !wait_over)
		return NULL;   /* the name waits for the identity page */

	const enum reac_box_family fam = reac_box_family_of(id);
	const char *said = (id && id->has_model_name) ? id->model_name : NULL;
	char name[REAC_BOX_NAME_MAX], token[REAC_BOX_NAME_TOKEN_MAX],
	     display[REAC_BOX_NAME_DISPLAY_MAX];
	if (reac_box_name(fam, said, declared_in, declared_out, name, sizeof name,
	                  token, sizeof token, display, sizeof display) != 0)
		return NULL;   /* not a box width */

	if (r->cur >= 0) {
		const struct reac_box_row_slot *last = &r->slot[r->cur];
		if (last->row.in_ch == declared_in && last->row.out_ch == declared_out &&
		    strcmp(last->display, display) == 0)
			return &last->row;
	}

	int next = r->cur == 0 ? 1 : 0;
	struct reac_box_row_slot *s = &r->slot[next];
	memset(s, 0, sizeof *s);
	memcpy(s->name, name, sizeof name);
	memcpy(s->token, token, sizeof token);
	memcpy(s->display, display, sizeof display);
	s->row.token = s->token;
	s->row.display = s->display;
	s->row.name = s->name;
	s->row.in_ch = declared_in;
	s->row.out_ch = declared_out;
	s->row.origin = REAC_BOX_DERIVED;
	r->cur = next;

	r->defect = reac_box_catalogue_defect(catalogue, declared_in, declared_out, display);
	if (r->defect)
		fprintf(stderr, "reac-pw: catalogue defect: the box declares %s, the catalogue "
		        "entry its declaration matches says %s (%d in / %d out) — the box wins\n",
		        display, catalogue->display, catalogue->in_ch, catalogue->out_ch);
	r->unknown_family = fam == REAC_BOX_FAMILY_UNKNOWN && !said;
	if (r->unknown_family) {
		if (have_hw)
			fprintf(stderr, "reac-pw: unknown hw family %02x%02x%02x%02x %02x%02x%02x%02x"
			        ": capture it — the box is named by its widths, %s\n",
			        id->reac_version_raw[0], id->reac_version_raw[1],
			        id->reac_version_raw[2], id->reac_version_raw[3],
			        id->reac_version_raw[4], id->reac_version_raw[5],
			        id->reac_version_raw[6], id->reac_version_raw[7], display);
		else
			fprintf(stderr, "reac-pw: unknown hw family: the box has not answered its "
			        "identity page — capture it; named by its widths, %s\n", display);
	}
	return &s->row;
}

int reac_box_row_has_playback(const struct reac_box_model *bm)
{
	return bm && bm->out_ch > 0;
}

void reac_box_row_badge_publish(const struct reac_box_model *bm, const char *link_state,
                                uint64_t box_mac48, const struct reac_identity *id,
                                reac_prop_set_fn set, void *ctx)
{
	if (!set)
		return;
	char width[16];
	if (bm)
		snprintf(width, sizeof width, "%dx%d", bm->in_ch, bm->out_ch);
	else
		snprintf(width, sizeof width, "0x0");
	if (link_state)
		set(ctx, REAC_PROP_LINK_STATE, link_state);
	set(ctx, REAC_PROP_BOX_MODEL, bm ? bm->token : "none");
	set(ctx, REAC_PROP_BOX_NAME, bm && bm->name ? bm->name : "");
	set(ctx, REAC_PROP_BOX_WIDTH, width);
	reac_box_mac_publish(box_mac48, set, ctx);
	reac_box_identity_publish(id, set, ctx);
}
