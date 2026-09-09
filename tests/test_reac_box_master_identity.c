// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* WHAT A JOINED BOX MASTER PUBLISHES ABOUT ITSELF (DESIGN.md 0.5.2).
 *
 * The rig, 2026-09-09 05:45: an S-0808 on M was joined, its eight channels were on the
 * graph, and the console showed a segment with NO STAGEBOX on it — because a console keys
 * a box off reac.box.mac / reac.box-model / reac.box-width / reac.link-state and the join
 * published none of the four. The box's inputs could not be patched, and the same chassis
 * is `box-676b3a9a` to that console when we master it.
 *
 * The two rules under test, and why they are rules rather than a formatter:
 *
 *   1. THE MODEL COMES FROM THE WIDTH, EXACTLY OR NOT AT ALL. A box on M sends no
 *      config-announce — reac_ctrl_identify_box has nothing to match — so the only thing
 *      that can name it is the geometry it broadcasts. libreac's reac_box_model_by_channels
 *      DEFAULTS to the S-1608 row for a width no model has, which would put a model name on
 *      a chassis nobody identified; reac_box_master_model refuses that outright.
 *   2. THE COMPOSITION AND THE STAMP ARE ONE ACT, tested through a recording fake, for the
 *      reason reac_link_state.h gives for reac.box.mac: what a consumer reads is the KEY
 *      and its VALUE, and a formatter tested alone leaves "is it written at all" — the half
 *      that was wrong on the rig — untested.
 */
#include "reac_link_state.h"
#include "reac_mac.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* The recording fake: every (key, value) the composer stamps, in order. */
struct rec {
	int  n;
	char key[8][48];
	char val[8][48];
};

static void rec_set(void *ctx, const char *key, const char *value)
{
	struct rec *r = ctx;
	if (r->n >= 8)
		return;
	snprintf(r->key[r->n], sizeof r->key[0], "%s", key);
	snprintf(r->val[r->n], sizeof r->val[0], "%s", value);
	r->n++;
}

/* The value stamped for `key`, or NULL when the composer never wrote it. ABSENCE IS THE
 * ANSWER for a width that names no model, so the test needs to read it as one. */
static const char *rec_get(const struct rec *r, const char *key)
{
	for (int i = 0; i < r->n; i++)
		if (strcmp(r->key[i], key) == 0)
			return r->val[i];
	return NULL;
}

int main(void)
{
	/* ---- 1. the width names the model, exactly ---------------------------------- */
	const struct reac_box_model *m8 = reac_box_master_model(8);
	CHK(m8 != NULL);
	CHK(m8 && strcmp(m8->token, "s0808") == 0);
	CHK(m8 && m8->in_ch == 8 && m8->out_ch == 8);

	const struct reac_box_model *m16 = reac_box_master_model(16);
	CHK(m16 && strcmp(m16->token, "s1608") == 0);
	const struct reac_box_model *m32 = reac_box_master_model(32);
	CHK(m32 && strcmp(m32->token, "s4000s") == 0);

	/* THE DEFAULT THAT MUST NOT HAPPEN. reac_box_model_by_channels answers the S-1608
	 * row for every one of these; this function answers nothing at all. */
	CHK(reac_box_master_model(0) == NULL);
	CHK(reac_box_master_model(7) == NULL);
	CHK(reac_box_master_model(9) == NULL);
	CHK(reac_box_master_model(40) == NULL);
	CHK(reac_box_master_model(64) == NULL);

	/* ---- 2. the rig's own case, stamped ----------------------------------------- */
	{
		struct rec r = { 0 };
		uint8_t mac[6] = { 0x00, 0x40, 0xab, 0xc4, 0xdc, 0x9c };  /* the rig's S-0808 */
		reac_box_master_identity_publish(8, reac_mac48_pack(mac), 1, rec_set, &r);

		const char *ls = rec_get(&r, REAC_PROP_LINK_STATE);
		const char *md = rec_get(&r, REAC_PROP_BOX_MODEL);
		const char *w  = rec_get(&r, REAC_PROP_BOX_WIDTH);
		const char *mc = rec_get(&r, REAC_PROP_BOX_MAC);
		CHK(ls && strcmp(ls, "established") == 0);
		CHK(md && strcmp(md, "s0808") == 0);
		/* The recognised model's OWN geometry, the same string the master side
		 * publishes for the same chassis — a console must fold the two into one box. */
		CHK(w && strcmp(w, "8x8") == 0);
		CHK(mc && strcmp(mc, "00:40:ab:c4:dc:9c") == 0);
	}

	/* ---- 3. not locked yet: probing, and nothing claims otherwise --------------- */
	{
		struct rec r = { 0 };
		uint8_t mac[6] = { 0x00, 0x40, 0xab, 0xc4, 0x08, 0xbc };
		reac_box_master_identity_publish(8, reac_mac48_pack(mac), 0, rec_set, &r);
		const char *ls = rec_get(&r, REAC_PROP_LINK_STATE);
		CHK(ls && strcmp(ls, "probing") == 0);
		/* The identity is known from the sighting before the stream locks — the width
		 * and the address are what the verdict was made from — so it is published
		 * with the honest link state beside it, not withheld. */
		CHK(rec_get(&r, REAC_PROP_BOX_MODEL) != NULL);
		CHK(rec_get(&r, REAC_PROP_BOX_MAC) != NULL);
	}

	/* ---- 4. a width no model has publishes NO model and NO width ---------------- */
	{
		struct rec r = { 0 };
		uint8_t mac[6] = { 0x00, 0x40, 0xab, 0x11, 0x22, 0x33 };
		reac_box_master_identity_publish(12, reac_mac48_pack(mac), 1, rec_set, &r);
		CHK(rec_get(&r, REAC_PROP_BOX_MODEL) == NULL);
		CHK(rec_get(&r, REAC_PROP_BOX_WIDTH) == NULL);
		/* What IS known is still said: whose clock, and that it is locked. */
		CHK(rec_get(&r, REAC_PROP_BOX_MAC) != NULL);
		CHK(rec_get(&r, REAC_PROP_LINK_STATE) != NULL);
	}

	/* ---- 5. no box, no address: the sentinel, never a zero MAC ------------------ */
	{
		struct rec r = { 0 };
		reac_box_master_identity_publish(8, 0, 0, rec_set, &r);
		const char *mc = rec_get(&r, REAC_PROP_BOX_MAC);
		CHK(mc && strcmp(mc, REAC_BOX_MAC_NONE) == 0);
	}

	/* ---- 6. a NULL sink is a no-op, not a crash --------------------------------- */
	reac_box_master_identity_publish(8, 1, 1, NULL, NULL);

	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("OK: a joined box master publishes the box it is — model and width implied by "
	       "the broadcast geometry (exactly, or not at all), its own address, and a link "
	       "state that says whether the stream is locked\n");
	return 0;
}
