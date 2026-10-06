// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The row a segment's nodes are sized from. See reac_box_row.h. */
#include "reac_box_row.h"

#include "reac_facts_pw.h"   /* REAC_BOX_MIN/MAX_CHANNELS, REAC_BRAID_PAIR_CHANNELS */
#include <stdio.h>
#include <string.h>

void reac_box_rows_init(struct reac_box_rows *r)
{
	if (!r)
		return;
	memset(r, 0, sizeof *r);
	r->cur = -1;
}

static int width_ok(int n)
{
	return n == 0 || (n >= REAC_BOX_MIN_CHANNELS && n <= REAC_BOX_MAX_CHANNELS &&
	                  n % REAC_BRAID_PAIR_CHANNELS == 0);
}

const struct reac_box_model *reac_box_row_resolve(struct reac_box_rows *r,
                                                  const struct reac_box_model *matched,
                                                  int declared_in, int declared_out)
{
	if (matched)
		return matched;
	if (!r || (declared_in == 0 && declared_out == 0) ||
	    !width_ok(declared_in) || !width_ok(declared_out))
		return NULL;

	if (r->cur >= 0) {
		const struct reac_box_model *last = &r->slot[r->cur].row;
		if (last->in_ch == declared_in && last->out_ch == declared_out)
			return last;
	}

	int next = r->cur == 0 ? 1 : 0;
	struct reac_box_row_slot *s = &r->slot[next];
	memset(s, 0, sizeof *s);

	/* A Roland row with exactly these widths names it; FreeREAC rows never name a
	 * box somebody else built. */
	size_t n;
	const struct reac_box_model *t = reac_box_model_table(&n);
	const struct reac_box_model *same = NULL;
	for (size_t i = 0; i < n && !same; i++)
		if (t[i].identity_shape == REAC_BOX_IDENTITY_ROLAND &&
		    t[i].in_ch == declared_in && t[i].out_ch == declared_out)
			same = &t[i];
	if (same) {
		snprintf(s->token, sizeof s->token, "%s", same->token);
		snprintf(s->display, sizeof s->display, "%s", same->display);
	} else {
		snprintf(s->token, sizeof s->token, "s4000s-%02d%02d", declared_in, declared_out);
		snprintf(s->display, sizeof s->display, "S-4000S-%02d%02d (%d in / %d out)",
		         declared_in, declared_out, declared_in, declared_out);
	}
	s->row.token = s->token;
	s->row.display = s->display;
	s->row.in_ch = declared_in;
	s->row.out_ch = declared_out;
	s->row.origin = REAC_BOX_DERIVED;
	r->cur = next;
	return &s->row;
}

int reac_box_row_has_playback(const struct reac_box_model *bm)
{
	return bm && bm->out_ch > 0;
}
