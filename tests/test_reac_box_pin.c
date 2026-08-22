// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* --box MODEL[:LABEL] — the operator's PIN for a fixed installation.
 *
 * reac-pw is a daemon in its own right, and a permanent rig wants its patch to exist
 * before the box is powered: the nodes named and sized from boot, not appearing halfway
 * through a soundcheck. openmixer is the opposite case and autodetects; the two are
 * complementary, not alternatives (operator, 2026-08-22).
 *
 * WHAT THE PIN MAY NOT DO. `--box` was retired on 2026-08-05 for a real defect: it
 * pre-fabricated a complete 16-channel head-amp enrollment before anything had been seen,
 * so a box reaching GRANTING before declaring itself — which a cold-connect JOIN always
 * does, carrying no width — was granted slots it may not own. That box "links, streams
 * audio, and silently ignores every head-amp record". The pin restored here therefore
 * declares NODE GEOMETRY AND LABEL only. It never allocates a grant: `reac_master_set_box`
 * stays the one door in, and the wire keeps deciding what is enrolled.
 *
 * And where they disagree the WIRE WINS, said once — `reac_box_pin_notice` already existed
 * for exactly that, from the retirement commit.
 */
#include "reac_box_pin.h"

#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

int main(void)
{
	const struct reac_box_model *m;
	const char *label;

	/* 1. A bare model token resolves, and the label DEFAULTS to the model's display
	 * name — so `--box s1608` alone already names the node something an operator
	 * recognises in a patchbay. */
	CHK(reac_box_pin_parse("s1608", &m, &label) == 0);
	CHK(m != NULL && strcmp(m->token, "s1608") == 0);
	CHK(m->in_ch == 16 && m->out_ch == 8);
	CHK(strcmp(label, m->display) == 0);

	CHK(reac_box_pin_parse("s0808", &m, &label) == 0);
	CHK(m->in_ch == 8 && m->out_ch == 8);

	/* 2. MODEL:LABEL takes the operator's own name. This is the fixed-install case —
	 * "the box by the drum riser" reads better than "S-1608" when there are three. */
	CHK(reac_box_pin_parse("s1608:Drums", &m, &label) == 0);
	CHK(strcmp(m->token, "s1608") == 0);
	CHK(strcmp(label, "Drums") == 0);

	/* A label may contain colons; only the FIRST separates it from the model. */
	CHK(reac_box_pin_parse("s0808:Stage: left", &m, &label) == 0);
	CHK(strcmp(m->token, "s0808") == 0);
	CHK(strcmp(label, "Stage: left") == 0);

	/* 3. AN UNKNOWN MODEL IS REFUSED, never guessed. A pin is the operator asserting a
	 * fact; silently accepting a typo would size the nodes to something nobody chose,
	 * and the graph would look plausible while being wrong. */
	CHK(reac_box_pin_parse("s9999", &m, &label) != 0);
	CHK(reac_box_pin_parse("", &m, &label) != 0);
	CHK(reac_box_pin_parse(":Drums", &m, &label) != 0);   /* label with no model */
	CHK(reac_box_pin_parse(NULL, &m, &label) != 0);

	/* A model token is matched WHOLE: a prefix of a real token is not that model. */
	CHK(reac_box_pin_parse("s16", &m, &label) != 0);
	CHK(reac_box_pin_parse("s1608x", &m, &label) != 0);

	/* An EMPTY label is not a label — fall back to the display name rather than
	 * naming a node "". */
	CHK(reac_box_pin_parse("s1608:", &m, &label) == 0);
	CHK(strcmp(label, m->display) == 0);

	/* 4. THE WIRE WINS, AND SAYS SO ONCE. reac_box_pin_notice returns 1 exactly when a
	 * surviving pin disagrees with what was recognised, and consumes the pin so a
	 * disagreement cannot become wallpaper at frame rate. */
	const char *pin = "s0808:Drums";
	CHK(reac_box_pin_notice(&pin, "s1608") == 1);   /* disagrees -> report */
	CHK(pin == NULL);                               /* consumed */
	CHK(reac_box_pin_notice(&pin, "s1608") == 0);   /* and never again */

	pin = "s1608:Drums";
	CHK(reac_box_pin_notice(&pin, "s1608") == 0);   /* agrees -> silence */
	CHK(pin == NULL);

	printf("test_reac_box_pin: OK\n");
	return 0;
}
