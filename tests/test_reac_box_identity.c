// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac.box.reac_version — THE WHOLE SEAM, from an identity-page reply on the wire
 * to the key/value pair that lands on the node's properties.
 *
 * The box answers the grant sweep's identity poll with two numbers that look
 * alike and are not: the SYSTEM firmware at DT1 addr 0x0000 and the REAC PROTOCOL
 * version at addr 0x0600. An M-200i displays both — an S-1608 reads `REAC 2.302`
 * beside `Firmware 2.200`, an S-4000S-3208 reads `REAC 2.102` beside `Firmware
 * 2.500` (operator, off the console's own display, 2026-09-14) — so a consumer
 * that gets one where it expects the other reads a plausible wrong version and
 * has no way to tell.
 *
 * That is why this is the seam and not the formatter: the frames go through
 * reac_pacer_rx_ingest, the snapshot crosses the seqlock the way the property
 * poll takes it, and the REAL composer (reac_box_identity_publish, the one the
 * sink stamps through) writes into a recording fake props dict. pw_properties
 * lives behind libpipewire and no unit test here links it.
 *
 * NO SOCKET, SO IT NEVER SKIPS: the pacer is built by hand with handle = NULL,
 * the construction reac_pacer_rx_ingest is documented for. */
#include <reac/reac_link_state.h>
#include <reac/reac_identity.h>
#include <reac/transport/reac_pacer.h>
#include <reac/reac.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

#define CHK(c) do { if (!(c)) { \
	fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* What pw_properties would hold, with the same MERGE semantics, plus a write
 * counter so "stamped empty" can be told from "not stamped at all" — the
 * distinction the whole update_properties-merges argument turns on. */
#define FAKE_MAX 8
struct fake_props {
	char key[FAKE_MAX][40];
	char val[FAKE_MAX][40];
	int  n;
	int  writes;
};

static void fake_set(void *ctx, const char *key, const char *value)
{
	struct fake_props *f = ctx;
	f->writes++;
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
	return NULL;
}

static const uint8_t OUR[6] = { 0x00, 0x14, 0x5c, 0x9b, 0x28, 0x2d };
static const uint8_t BOX[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x3b };

/* One DT1 identity REPLY as the wire carries it: 88 19, type cd ea, then the
 * control block whose SysEx is f0 41 0a 00 00 12 12 <tag> <addr_lo> <payload>
 * <cksum> f7. The block checksum is a stand-in; reac_ctrl_parse reads structure. */
static void build_identity_reply(uint8_t *fr, uint16_t addr_lo,
                                 const uint8_t *pl, size_t n)
{
	memset(fr, 0, REAC_FRAME_BYTES);
	memcpy(fr, OUR, 6);
	memcpy(fr + 6, BOX, 6);
	fr[REAC_ETHERTYPE_OFF] = REAC_ETHERTYPE >> 8; fr[REAC_ETHERTYPE_OFF + 1] = REAC_ETHERTYPE & 0xff;
	fr[16] = 0xcd; fr[17] = 0xea;
	uint8_t *b = fr + REAC_CTRL_BLOCK_OFF;
	unsigned sx = (unsigned)(13 + n);
	b[0] = 0x04; b[1] = 0x03; b[3] = (uint8_t)(sx + 5);
	b[5] = 0x02; b[7] = 0xfe; b[8] = (uint8_t)sx;
	b[9] = 0xf0; b[10] = 0x41; b[11] = 0x0a; b[14] = 0x12; b[15] = 0x12;
	b[16] = 0x05; b[17] = 0x00;
	b[18] = (uint8_t)(addr_lo >> 8); b[19] = (uint8_t)(addr_lo & 0xff);
	for (size_t i = 0; i < n; i++)
		b[20 + i] = pl[i];
	b[20 + n] = 0x7f; b[21 + n] = 0xf7;
}

/* Stamp the badge exactly the way the sink stamps it: the pacer's snapshot
 * through the REAL composer. No formatting happens here — that is the point. */
static void publish(struct reac_pacer *p, struct fake_props *f)
{
	struct reac_identity id;
	reac_pacer_read_identity(p, &id);
	reac_box_identity_publish(&id, fake_set, f);
}

int main(void)
{
	/* The S-1608's two replies, and the S-4000S-3208's, as captured. */
	static const uint8_t FW_S1608[4]   = { 0x02, 0x02, 0x00, 0x00 };  /* 2.200 */
	static const uint8_t VER_S1608[8]  = { 0x00, 0x00, 0x00, 0x02, 0x00, 0x03, 0x00, 0x02 };
	static const uint8_t FW_S4000S[4]  = { 0x02, 0x05, 0x00, 0x00 };  /* 2.500 */
	static const uint8_t VER_S4000S[8] = { 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x02 };

	struct reac_pacer p;
	memset(&p, 0, sizeof p);
	p.handle = NULL;
	p.fps = REAC_PKT_RATE_96K;
	memcpy(p.src, OUR, 6);
	CHK(reac_frame_ring_init(&p.ring, REAC_BOX_S0808_IN, 2048) == 0);
	reac_master_init(&p.master, OUR, NULL, REAC_PKT_RATE_96K);

	uint8_t frame[REAC_FRAME_BYTES];
	struct fake_props f;
	memset(&f, 0, sizeof f);

	/* ---- 1. Nothing answered yet: all three keys are WRITTEN, and empty. An
	 * unwritten key would let a consumer keep whatever the dict last held. */
	publish(&p, &f);
	CHK(f.writes == 3 && f.n == 3);
	CHK(strcmp(fake_get(&f, "reac.box.reac_version"), "") == 0);
	CHK(strcmp(fake_get(&f, "reac.box-firmware"), "") == 0);
	CHK(strcmp(fake_get(&f, "reac.box-hw"), "") == 0);

	/* ---- 2. The S-1608 answers both addresses. The key a consumer matches is
	 * spelled out here, not taken from the macro: a rename must break a test
	 * rather than a rig. */
	build_identity_reply(frame, REAC_IDENTITY_ADDR_FIRMWARE, FW_S1608, 4);
	reac_pacer_rx_ingest(&p, frame, REAC_FRAME_BYTES);
	build_identity_reply(frame, REAC_IDENTITY_ADDR_REAC_VERSION, VER_S1608, 8);
	reac_pacer_rx_ingest(&p, frame, REAC_FRAME_BYTES);
	publish(&p, &f);
	CHK(strcmp(fake_get(&f, "reac.box.reac_version"), "2.302") == 0);
	CHK(strcmp(fake_get(&f, "reac.box-firmware"), "2.200") == 0);
	CHK(strcmp(fake_get(&f, "reac.box-hw"), "00000002 00030002") == 0);
	/* THE TWO NUMBERS ARE NOT THE SAME NUMBER — the defect this test exists for
	 * is one standing in for the other, and both are plausible versions. */
	CHK(strcmp(fake_get(&f, "reac.box.reac_version"),
	           fake_get(&f, "reac.box-firmware")) != 0);

	/* ---- 3. An S-4000S-3208 on the same segment: a different REAC version off
	 * the same address, and every key re-stamped over the previous box's. */
	build_identity_reply(frame, REAC_IDENTITY_ADDR_FIRMWARE, FW_S4000S, 4);
	reac_pacer_rx_ingest(&p, frame, REAC_FRAME_BYTES);
	build_identity_reply(frame, REAC_IDENTITY_ADDR_REAC_VERSION, VER_S4000S, 8);
	reac_pacer_rx_ingest(&p, frame, REAC_FRAME_BYTES);
	publish(&p, &f);
	CHK(strcmp(fake_get(&f, "reac.box.reac_version"), "2.102") == 0);
	CHK(strcmp(fake_get(&f, "reac.box-firmware"), "2.500") == 0);
	CHK(f.n == 3);   /* still three keys, not a growing set */

	reac_frame_ring_free(&p.ring);
	printf("OK: reac.box.reac_version — the identity page's 0x0600 record reaches "
	       "the node property as the console prints it (S-1608 2.302 beside firmware "
	       "2.200, S-4000S-3208 2.102 beside 2.500), and an unanswered page is "
	       "stamped empty rather than left standing\n");
	return 0;
}
