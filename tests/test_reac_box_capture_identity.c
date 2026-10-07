// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* A BOX NO ROW NAMES STILL ASKS FOR ITS IDENTITY AND PUBLISHES IT, WHATEVER ITS WIDTH.
 *
 * The desk, 2026-10-06: /reac/segment read firmware 2.200 and hw block
 * 00000002 00030002 for the S-1608 (a matched row, with a reac-playback) and empty
 * patchName, firmware and hwBlock for the S-4000S-4000 (00:40:ab:c4:06:80, 40 in /
 * 0 out, no row). That box has no reac-playback, and only reac-playback was ever
 * stamped with the identity page; reac-capture, its only node, carried none of it.
 *
 *   1. its declaration (cells 02 x10, 03 x2) gives a row of 40 / 0 and no playback;
 *   2. the master granting it at 40 sends the identity poll: the sweep carries the
 *      six group-B RQ1 records on DT1 tag 0x0500, as for any matched box;
 *   3. its identity replies, folded by the pacer, reach the badge reac-capture is
 *      stamped with (reac_box_row_badge_publish): firmware, REAC version, hw block,
 *      the box's address, model and width.
 *
 * No socket and no PipeWire: the pacer is built by hand with handle = NULL, and the
 * badge goes into a recording fake props dict, as in test_reac_box_identity.c. */
#include <reac/reac_ctrlblk.h>
#include <reac/reac_ports.h>
#include <reac/reac_master.h>
#include <reac/reac_identity.h>
#include <reac/reac_link_state.h>
#include <reac/transport/reac_segment_ident.h>   /* reac_mac48_pack */
#include <reac/transport/reac_pacer.h>
#include <reac/reac.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "reac_facts_pw.h"
#include "reac_box_row.h"

static int fails;
#define CHK(c) do { \
	if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
} while (0)

#define FAKE_MAX 16
struct fake_props {
	char key[FAKE_MAX][40];
	char val[FAKE_MAX][40];
	int  n;
};

static void fake_set(void *ctx, const char *key, const char *value)
{
	struct fake_props *f = ctx;
	for (int i = 0; i < f->n; i++)
		if (strcmp(f->key[i], key) == 0) {
			snprintf(f->val[i], sizeof f->val[i], "%s", value);
			return;
		}
	if (f->n >= FAKE_MAX)
		return;
	snprintf(f->key[f->n], sizeof f->key[f->n], "%s", key);
	snprintf(f->val[f->n], sizeof f->val[f->n], "%s", value);
	f->n++;
}

static const char *fake_get(const struct fake_props *f, const char *key)
{
	for (int i = 0; i < f->n; i++)
		if (strcmp(f->key[i], key) == 0)
			return f->val[i];
	return "";
}

static const uint8_t OUR[6] = { 0x00, 0x14, 0x5c, 0x9b, 0x28, 0x2d };
static const uint8_t BOX[6] = { 0x00, 0x40, 0xab, 0xc4, 0x06, 0x80 };

/* The box's declaration: link 1, SINGLE, length 0x10, opcode 0x84, strap 0x00, cells
 * 02 x10 | 03 x2. */
static const uint8_t DECL_4000[32] = {
	0x01, 0x03, 0x00, 0x10, 0x84, 0x00, 0x00, 0x00,
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x03, 0x03,
};

static void build_identity_reply(uint8_t *fr, uint16_t addr_lo,
                                 const uint8_t *pl, size_t n)
{
	memset(fr, 0, REAC_FRAME_BYTES);
	memcpy(fr, OUR, 6);
	memcpy(fr + 6, BOX, 6);
	fr[REAC_ETHERTYPE_OFF] = REAC_ETHERTYPE >> 8; fr[REAC_ETHERTYPE_OFF + 1] = REAC_ETHERTYPE & 0xff;
	fr[REAC_TYPED_BLOCK_OFF] = REAC_TYPE_CONTROL >> 8; fr[REAC_TYPED_BLOCK_OFF + 1] = REAC_TYPE_CONTROL & 0xff;
	reacpw_dt1_record(fr + REAC_CTRL_BLOCK_OFF, REAC_DT1_TAG_IDENTITY, addr_lo, pl, n);
}

int main(void)
{
	/* ---- 1. The declaration: 40 / 0, no row, no playback. */
	struct reac_box_ports ports;
	CHK(reac_ports_parse(DECL_4000, &ports) == 0);
	CHK(ports.in_ch == 40 && ports.out_ch == 0);
	struct reac_box_rows rows;
	reac_box_rows_init(&rows);
	const struct reac_box_model *bm = reac_box_row_resolve(&rows, NULL, ports.in_ch,
	                                                       ports.out_ch, NULL, 1);
	CHK(bm != NULL);
	CHK(bm && !reac_box_row_has_playback(bm));

	/* ---- 2. The request: granted at 40, the sweep carries the identity poll. */
	static struct reac_master m;
	reac_master_init(&m, OUR, NULL, REAC_PKT_RATE_96K);
	reac_master_set_box(&m, ports.in_ch, ports.out_ch, ports.headamp_base);
	CHK(reac_master_has_box(&m));
	CHK(m.alloc.width == 40);
	int polls = 0;
	for (int i = 0; i < m.grant_burst_len; i++) {
		const uint8_t *r = m.grant_burst[i];
		/* The SysEx at row[10..]: f0 41 0a 00 00 12 11 <tag 05 00>: RQ1, identity page. */
		if (r[16] == 0x12 && r[17] == 0x11 &&
		    ((unsigned)r[18] << 8 | r[19]) == REAC_DT1_TAG_IDENTITY)
			polls++;
	}
	CHK(polls == 6);

	/* ---- 3. The answer reaches the badge reac-capture is stamped with. */
	static struct reac_pacer p;
	memset(&p, 0, sizeof p);
	p.handle = NULL;
	p.fps = REAC_PKT_RATE_96K;
	memcpy(p.src, OUR, 6);
	CHK(reac_frame_ring_init(&p.ring, REAC_BOX_S0808_IN, 2048) == 0);
	reac_master_init(&p.master, OUR, NULL, REAC_PKT_RATE_96K);

	static const uint8_t FW[REAC_IDENTITY_FIRMWARE_BYTES]       = { 0x02, 0x05, 0x00, 0x00 };
	static const uint8_t VER[REAC_IDENTITY_REAC_VERSION_BYTES]  = { 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x02 };
	uint8_t frame[REAC_FRAME_BYTES];
	build_identity_reply(frame, REAC_IDENTITY_ADDR_FIRMWARE_VERSION, FW, sizeof FW);
	reac_pacer_rx_ingest(&p, frame, REAC_FRAME_BYTES);
	build_identity_reply(frame, REAC_IDENTITY_ADDR_REAC_VERSION, VER, sizeof VER);
	reac_pacer_rx_ingest(&p, frame, REAC_FRAME_BYTES);

	struct reac_identity id;
	reac_pacer_read_identity(&p, &id);
	/* The name comes from the page that just arrived: the S-4000S hw block and 40 / 0. */
	bm = reac_box_row_resolve(&rows, NULL, ports.in_ch, ports.out_ch, &id, 0);
	CHK(bm != NULL);
	struct fake_props f;
	memset(&f, 0, sizeof f);
	reac_box_row_badge_publish(bm, "established", reac_mac48_pack(BOX), &id, fake_set, &f);

	CHK(strcmp(fake_get(&f, "reac.box-firmware"), "2.500") == 0);
	CHK(strcmp(fake_get(&f, "reac.box.reac_version"), "2.102") == 0);
	CHK(strcmp(fake_get(&f, "reac.box-hw"), "00000002 00010002") == 0);
	CHK(strcmp(fake_get(&f, "reac.box.mac"), "00:40:ab:c4:06:80") == 0);
	CHK(strcmp(fake_get(&f, "reac.box-model"), "s4000s-4000") == 0);
	CHK(strcmp(fake_get(&f, "reac.box-name"), "S-4000S-4000") == 0);
	CHK(strcmp(fake_get(&f, "reac.box-width"), "40x0") == 0);
	CHK(strcmp(fake_get(&f, "reac.link-state"), "established") == 0);

	/* The box leaves: every key is stamped back to "no box", none left standing. */
	struct reac_identity none;
	reac_identity_init(&none);
	reac_box_row_badge_publish(NULL, "probing", 0, &none, fake_set, &f);
	CHK(strcmp(fake_get(&f, "reac.box-firmware"), "") == 0);
	CHK(strcmp(fake_get(&f, "reac.box-hw"), "") == 0);
	CHK(strcmp(fake_get(&f, "reac.box-model"), "none") == 0);
	CHK(strcmp(fake_get(&f, "reac.box-width"), "0x0") == 0);

	reac_frame_ring_free(&p.ring);
	if (fails) {
		fprintf(stderr, "test_reac_box_capture_identity: %d failure(s)\n", fails);
		return 1;
	}
	printf("OK: a 40 in / 0 out box no row names is granted 40 wide with the identity "
	       "poll in its sweep, and its firmware, REAC version, hw block and address reach "
	       "the badge its reac-capture carries\n");
	return 0;
}
