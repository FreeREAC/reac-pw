// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* A BOX NO ROW NAMES IS SIZED AND NAMED FROM ITS DECLARATION.
 *
 * The desk, 2026-10-06: a stock S-4000S-3208 re-fitted to 16 in / 24 out (box
 * 00:40:ab:c4:08:bc) declared itself with no matrix row matching, and reac-pw built
 * neither reac-capture nor reac-playback. Its declaration, captured at 17:54:50
 * (s4000s-1624-announce2.pcap frame 32), is below; libreac's reac_ports_parse reads its
 * widths exactly as the pacer does, and the row the nodes are sized from must be
 * capture 16 / playback 24, named S-4000S-1624. A row that matched is returned as is. */
#include <reac/reac_ctrlblk.h>
#include <reac/reac_ports.h>

#include <stdio.h>
#include <string.h>

#include "reac_box_row.h"

static int fails;
#define CHK(c) do { \
	if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
} while (0)

/* frame[18:50], the control block: link 1, SINGLE, length 0x10, opcode 0x84, strap 0x00,
 * cells 02 02 02 02 | 01 01 01 01 01 01 | 03 03, check byte 0x50. */
static const uint8_t DECL_1624[32] = {
	0x01, 0x03, 0x00, 0x10, 0x84, 0x00, 0x00, 0x00,
	0x02, 0x02, 0x02, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x03, 0x03,
	0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50,
};

int main(void)
{
	struct reac_box_rows rows;
	reac_box_rows_init(&rows);

	struct reac_box_ports ports;
	CHK(reac_ports_parse(DECL_1624, &ports) == 0);

	const struct reac_box_model *bm = reac_box_row_resolve(&rows, NULL, ports.in_ch,
	                                                       ports.out_ch);
	CHK(bm != NULL);
	if (bm) {
		CHK(bm->in_ch == 16);    /* reac-capture  */
		CHK(bm->out_ch == 24);   /* reac-playback */
		CHK(strcmp(bm->token, "s4000s-1624") == 0);
		CHK(strcmp(bm->display, "S-4000S-1624 (16 in / 24 out)") == 0);
	}
	/* The box repeats its declaration: the same row, so the nodes are not rebuilt. */
	CHK(reac_box_row_resolve(&rows, NULL, ports.in_ch, ports.out_ch) == bm);

	/* A matched row is the row, unchanged; no declaration is no row. */
	const struct reac_box_model *s1608 = reac_box_model_by_token("s1608");
	CHK(s1608 && reac_box_row_resolve(&rows, s1608, 16, 8) == s1608);
	CHK(reac_box_row_resolve(&rows, NULL, 0, 0) == NULL);

	/* A box that declares 40 inputs / 0 outputs (cells 02 x10, 03 x2): capture 40 and
	 * no playback node at all. */
	static const uint8_t DECL_4000[32] = {
		0x01, 0x03, 0x00, 0x10, 0x84, 0x00, 0x00, 0x00,
		0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x03, 0x03,
	};
	CHK(reac_ports_parse(DECL_4000, &ports) == 0);
	const struct reac_box_model *in_only = reac_box_row_resolve(&rows, NULL, ports.in_ch,
	                                                            ports.out_ch);
	CHK(in_only != NULL);
	if (in_only) {
		CHK(in_only->in_ch == 40);
		CHK(in_only->out_ch == 0);
		CHK(!reac_box_row_has_playback(in_only));
		CHK(strcmp(in_only->token, "s4000s-4000") == 0);
	}
	CHK(bm && reac_box_row_has_playback(bm));   /* the 16 / 24 box keeps its 24 */

	if (fails) {
		fprintf(stderr, "test_reac_box_row: %d failure(s)\n", fails);
		return 1;
	}
	printf("OK: the re-fitted box's declaration sizes reac-capture 16 / reac-playback 24 "
	       "and names it S-4000S-1624; 40 / 0 gives capture 40 and no playback; a matched "
	       "row is returned unchanged\n");
	return 0;
}
