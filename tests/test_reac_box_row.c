// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* EVERY BOX IS SIZED AND NAMED FROM ITS FRAMES (FreeREAC/reac-pw#5, ruling 2026-10-07).
 *
 *   1. A declaration that differs from the catalogue entry it matches is sized and named
 *      by the declaration, and the entry is reported as a catalogue defect.
 *   2. The name and the firmware come from the identity page: the hw block names the
 *      family, the S-0808's own name record names it, and reac.box-firmware / reac.box-hw
 *      / reac.box-name are stamped from it.
 *   3. The captured 16 / 24 declaration (s4000s-1624-announce2.pcap, the re-fitted
 *      S-4000S 00:40:ab:c4:08:bc) is capture 16 / playback 24, S-4000S-1624; a 40 / 0
 *      declaration (synthesised per reac.ksy: cells 02 x10, 03 x2) is capture 40 and no
 *      playback node.
 *   4. The name waits for the identity page, then names an unknown family by widths. */
#include <reac/reac_box_facts.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_identity.h>
#include <reac/reac_link_state.h>
#include <reac/reac_ports.h>

#include <stdio.h>
#include <string.h>

#include "reac_box_row.h"

static int fails;
#define CHK(c) do { \
	if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
} while (0)

/* frame[18:50] of the captured 16 / 24 announce. */
static const uint8_t DECL_1624[32] = {
	0x01, 0x03, 0x00, 0x10, 0x84, 0x00, 0x00, 0x00,
	0x02, 0x02, 0x02, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x03, 0x03,
	0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50,
};
static const uint8_t DECL_4000[32] = {
	0x01, 0x03, 0x00, 0x10, 0x84, 0x00, 0x00, 0x00,
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x03, 0x03,
};
/* The identity pages the corpus holds (0x0000 firmware, 0x0600 hw block). */
static const uint8_t FW_2500[4] = { 2, 5, 0, 0 };
static const uint8_t FW_1003[4] = { 1, 0, 0, 3 };
static const uint8_t HW_S4000S[8] = { 0, 0, 0, 2, 0, 1, 0, 2 };
static const uint8_t HW_S1608[8]  = { 0, 0, 0, 2, 0, 3, 0, 2 };
static const uint8_t HW_S0808[8]  = { 0, 0, 0, 1, 0, 0, 0, 0 };
static const uint8_t HW_NEW[8]    = { 0, 0, 0, 2, 0, 9, 0, 9 };
static const uint8_t NAME_S0808[17] = { 0x01, 'S', '-', '0', '8', '0', '8' };

static void page(struct reac_identity *id, const uint8_t *fw, const uint8_t *hw)
{
	reac_identity_init(id);
	if (fw)
		reac_identity_ingest(id, REAC_IDENTITY_ADDR_FIRMWARE, fw, 4);
	if (hw)
		reac_identity_ingest(id, REAC_IDENTITY_ADDR_REAC_VERSION, hw, 8);
}

struct props { char model[32], name[32], width[16], fw[16], hw[32]; };
static void put(void *ctx, const char *k, const char *v)
{
	struct props *p = ctx;
	if (!strcmp(k, REAC_PROP_BOX_MODEL)) snprintf(p->model, sizeof p->model, "%s", v);
	if (!strcmp(k, REAC_PROP_BOX_NAME)) snprintf(p->name, sizeof p->name, "%s", v);
	if (!strcmp(k, REAC_PROP_BOX_WIDTH)) snprintf(p->width, sizeof p->width, "%s", v);
	if (!strcmp(k, REAC_PROP_BOX_FIRMWARE)) snprintf(p->fw, sizeof p->fw, "%s", v);
	if (!strcmp(k, REAC_PROP_BOX_HW)) snprintf(p->hw, sizeof p->hw, "%s", v);
}

int main(void)
{
	struct reac_box_rows rows;
	struct reac_identity id;
	struct reac_box_ports ports;
	const struct reac_box_model *bm;

	/* ---- 3. the captured 16 / 24 box, its S-4000S identity page ---- */
	reac_box_rows_init(&rows);
	CHK(reac_ports_parse(DECL_1624, &ports) == 0);
	page(&id, FW_2500, HW_S4000S);
	bm = reac_box_row_resolve(&rows, NULL, ports.in_ch, ports.out_ch, &id, 0);
	CHK(bm != NULL);
	if (bm) {
		CHK(bm->in_ch == 16 && bm->out_ch == 24);
		CHK(strcmp(bm->token, "s4000s-1624") == 0);
		CHK(strcmp(bm->name, "S-4000S-1624") == 0);
		CHK(strcmp(bm->display, "S-4000S-1624 (16 in / 24 out)") == 0);
		CHK(reac_box_row_has_playback(bm));
	}
	/* the box repeats itself: the same row, so the nodes are not rebuilt */
	CHK(reac_box_row_resolve(&rows, NULL, ports.in_ch, ports.out_ch, &id, 0) == bm);

	/* ---- 2. name and firmware are what the identity page says ---- */
	{
		struct props p;
		memset(&p, 0, sizeof p);
		reac_box_row_badge_publish(bm, NULL, 0, &id, put, &p);
		CHK(strcmp(p.model, "s4000s-1624") == 0);
		CHK(strcmp(p.name, "S-4000S-1624") == 0);
		CHK(strcmp(p.width, "16x24") == 0);
		CHK(strcmp(p.fw, "2.500") == 0);
		CHK(strcmp(p.hw, "00000002 00010002") == 0);
		memset(&p, 0, sizeof p);
		reac_box_row_badge_publish(NULL, NULL, 0, NULL, put, &p);   /* box gone */
		CHK(strcmp(p.model, "none") == 0 && p.name[0] == 0 && p.fw[0] == 0);
	}
	/* the S-0808 says its own name */
	reac_box_rows_init(&rows);
	page(&id, FW_1003, HW_S0808);
	reac_identity_ingest(&id, REAC_IDENTITY_ADDR_MODEL_NAME, NAME_S0808, sizeof NAME_S0808);
	bm = reac_box_row_resolve(&rows, NULL, 8, 8, &id, 0);
	CHK(bm && strcmp(bm->name, "S-0808") == 0 && !rows.unknown_family);

	/* ---- 1. a catalogue entry that disagrees: the declaration wins, defect logged ---- */
	{
		const struct reac_box_model *s1608 = reac_box_catalogue_by_token("s1608");
		CHK(s1608 != NULL);
		reac_box_rows_init(&rows);
		page(&id, NULL, HW_S1608);
		/* an S-1608 hw block, a stock 16 / 8 declaration: no defect */
		bm = reac_box_row_resolve(&rows, s1608, 16, 8, &id, 0);
		CHK(bm && bm != s1608 && strcmp(bm->name, "S-1608") == 0 && rows.defect == 0);
		/* the same entry against a declaration of 16 / 24 */
		bm = reac_box_row_resolve(&rows, s1608, 16, 24, &id, 0);
		CHK(bm && bm->in_ch == 16 && bm->out_ch == 24);
		CHK(bm && strcmp(bm->name, "S-1608-1624") == 0);
		CHK(rows.defect == (REAC_BOX_DEFECT_WIDTH | REAC_BOX_DEFECT_NAME));
	}

	/* ---- 3b. 40 in / 0 out: capture 40, no playback ---- */
	reac_box_rows_init(&rows);
	CHK(reac_ports_parse(DECL_4000, &ports) == 0);
	page(&id, FW_2500, HW_S4000S);
	bm = reac_box_row_resolve(&rows, NULL, ports.in_ch, ports.out_ch, &id, 0);
	CHK(bm && bm->in_ch == 40 && bm->out_ch == 0 && !reac_box_row_has_playback(bm));
	CHK(bm && strcmp(bm->name, "S-4000S-4000") == 0);

	/* ---- 4. the name waits for the identity page ---- */
	reac_box_rows_init(&rows);
	page(&id, NULL, NULL);
	CHK(reac_box_row_resolve(&rows, NULL, 8, 16, &id, 0) == NULL);   /* still waiting */
	bm = reac_box_row_resolve(&rows, NULL, 8, 16, &id, 1);          /* waited enough */
	CHK(bm && strcmp(bm->name, "REAC-0816") == 0 && rows.unknown_family);
	page(&id, NULL, HW_NEW);                                         /* a family nobody captured */
	reac_box_rows_init(&rows);
	bm = reac_box_row_resolve(&rows, NULL, 8, 16, &id, 0);
	CHK(bm && strcmp(bm->name, "REAC-0816") == 0 && rows.unknown_family);
	CHK(reac_box_row_resolve(&rows, NULL, 0, 0, &id, 1) == NULL);   /* nothing declared */

	if (fails) {
		fprintf(stderr, "test_reac_box_row: %d failure(s)\n", fails);
		return 1;
	}
	printf("OK: boxes are sized by their declaration and named by their hw family and "
	       "widths; firmware and hw block come from the identity page; a disagreeing "
	       "catalogue entry is a defect; 40 / 0 has no playback\n");
	return 0;
}
