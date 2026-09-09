// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <reac/reac_boxreg.h>
#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static int mac_all_zero(const struct reac_boxreg *r, int idx);

static const uint8_t A[6] = { 0x00,0x40,0xab,0xc4,0x80,0x3b };  /* S-1608 */
static const uint8_t B[6] = { 0x00,0x40,0xab,0xc4,0xdc,0x9c };  /* S-0808 */
static const uint8_t C[6] = { 0x00,0x40,0xab,0xc4,0x06,0x80 };  /* S-4000S */

int main(void)
{
	struct reac_boxreg r;

	/* --- auto-allocation: contiguous, by join order --- */
	reac_boxreg_init(&r, 40);
	CHK(r.n == 0 && r.fabric == 40);
	int ia = reac_boxreg_add(&r, A, 16);          /* S-1608 -> slots 0..15  */
	CHK(ia == 0 && r.box[0].base == 0 && r.box[0].nch == 16);
	int ib = reac_boxreg_add(&r, B, 8);           /* S-0808 -> slots 16..23 */
	CHK(ib == 1 && r.box[1].base == 16 && r.box[1].nch == 8);
	int ic = reac_boxreg_add(&r, C, 8);           /* S-0808 -> slots 24..31 */
	CHK(ic == 2 && r.box[2].base == 24);

	/* find + idempotency */
	CHK(reac_boxreg_find(&r, A) == 0 && reac_boxreg_find(&r, B) == 1);
	CHK(reac_boxreg_add(&r, A, 16) == 0);         /* re-add same MAC = same idx */
	CHK(r.n == 3);

	/* a zero MAC never matches a real box */
	uint8_t zero[6] = {0};
	CHK(reac_boxreg_find(&r, zero) == -1);

	/* invalid widths rejected (odd, too big, too small) */
	uint8_t D[6] = { 1,2,3,4,5,6 };
	CHK(reac_boxreg_add(&r, D, 7) == -1);
	CHK(reac_boxreg_add(&r, D, 0) == -1);
	CHK(reac_boxreg_add(&r, D, 42) == -1);

	/* --- gap reuse: a departed box's slots are reclaimed by the next add --- */
	struct reac_boxreg g;
	reac_boxreg_init(&g, 40);
	reac_boxreg_add(&g, A, 16);   /* 0..15 */
	reac_boxreg_add(&g, B, 8);    /* 16..23 */
	/* simulate box A leaving: remove it by compacting (emulate a future remove) */
	g.box[0] = g.box[1]; g.n = 1;                 /* now only B at base 16    */
	int nb = reac_boxreg_add(&g, C, 16);          /* lowest free 16-wide = 0  */
	CHK(nb == 1 && g.box[1].base == 0);

	/* --- pre-declared names + pinned base bind on JOIN --- */
	struct reac_boxreg d;
	reac_boxreg_init(&d, 40);
	int dd = reac_boxreg_declare(&d, 16, "Drums", -1);      /* auto base 0 */
	CHK(dd == 0 && d.box[0].base == 0 && strcmp(d.box[0].name, "Drums") == 0);
	CHK(mac_all_zero(&d, 0));                                /* still unbound */
	int vv = reac_boxreg_declare(&d, 8, "Vocals", 24);      /* pinned base 24 */
	CHK(vv == 1 && d.box[1].base == 24 && d.box[1].pinned);
	/* a 16-wide box JOINs -> binds to the "Drums" slot (matching width) */
	int j = reac_boxreg_add(&d, A, 16);
	CHK(j == 0 && memcmp(d.box[0].mac, A, 6) == 0 && strcmp(d.box[0].name, "Drums") == 0);
	/* an 8-wide box JOINs -> binds to the pinned "Vocals" slot at base 24 */
	int j2 = reac_boxreg_add(&d, B, 8);
	CHK(j2 == 1 && d.box[1].base == 24 && strcmp(d.box[1].name, "Vocals") == 0);

	/* pinned collision rejected */
	CHK(reac_boxreg_declare(&d, 8, "X", 0) == -1);          /* base 0 already taken */

	/* set_name overwrites */
	reac_boxreg_set_name(&d, 0, "Kit");
	CHK(strcmp(d.box[0].name, "Kit") == 0);

	/* --- fabric full: no room for a 6th box beyond 40 slots --- */
	struct reac_boxreg f;
	reac_boxreg_init(&f, 40);
	uint8_t m[6] = { 0x00,0x40,0xab,0,0,0 };
	int placed = 0;
	for (int i = 0; i < 8; i++) { m[5] = (uint8_t)i; if (reac_boxreg_add(&f, m, 8) >= 0) placed++; }
	CHK(placed == 5);                                       /* 5 x 8 = 40, 6th rejected */

	printf("test_reac_boxreg: OK\n");
	return 0;
}

/* helper referenced above: is box idx's MAC still unbound (all zero)? */
static int mac_all_zero(const struct reac_boxreg *r, int idx)
{
	for (int k = 0; k < 6; k++)
		if (r->box[idx].mac[k])
			return 0;
	return 1;
}
