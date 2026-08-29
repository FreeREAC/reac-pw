// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The chanmap section marker carries the CONSOLE FAMILY, not a constant.
 *
 * A real M-5000 writes the marker slot `fe 01 00` where an M-200/M-300 writes
 * `fe 00 00` (reac-captures: m5000-s1608-96k / m5000-s0808-96k against the m200i
 * 48k sessions, 2026-08-29). reac-pw emitted the V-Mixer form at every rate, so a
 * box driven under our OHRCA impersonation — cfea[19] = 1 since 0c0f4c9 — saw an
 * OHRCA console announce over an M-200's channel map. The S-1608 (fw 2.200) has
 * never followed that impersonation to 96 kHz; the S-0808 (fw 1.003) does.
 *
 * The two properties this pins are equally load-bearing:
 *
 *   1. OHRCA (console_field 1) emits `fe 01 00`.
 *   2. V-Mixer (console_field 0) is BYTE-IDENTICAL to before, and the two builds
 *      differ in EXACTLY the marker byte — one byte, in the one window that holds
 *      the marker, checksum aside. This is an addition to what we can say, not a
 *      change to what we already said: tests/test_reac_s1608.c pins the captured
 *      M-200 windows byte-for-byte, and it must stay green.
 *
 * The marker rides ring position 48 (the wrap), so it appears in the frame whose
 * window starts there plus every window that runs through the wrap — the count is
 * asserted rather than assumed, because a generator that stopped emitting the
 * marker entirely would otherwise pass property 1 vacuously. */
#include "reac_master.h"

#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static const uint8_t SRC[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };
#define FPS 8000

/* Slot s of a chanmap block: 3 bytes at [7 + 3s]. */
#define SLOT(blk, s) ((blk) + 7 + (s) * 3)

static int count_marker_slots(const struct reac_master *m, uint8_t want_family)
{
	int n = 0;
	for (int f = 0; f < m->chanmap_nframes; f++)
		for (int s = 0; s < 8; s++) {
			const uint8_t *t = SLOT(m->chanmap[f], s);
			if (t[0] == 0xfe && t[1] == want_family && t[2] == 0x00)
				n++;
		}
	return n;
}

int main(void)
{
	struct reac_master vmix, ohrca;
	struct reac_console_cfg cfg_v = REAC_CONSOLE_CFG_IDLE;
	struct reac_console_cfg cfg_o = REAC_CONSOLE_CFG_IDLE;
	cfg_v.console_field = 0;
	cfg_o.console_field = 1;

	reac_master_init(&vmix,  SRC, &cfg_v, FPS);
	reac_master_init(&ohrca, SRC, &cfg_o, FPS);

	/* The sweep itself is unchanged: same window count either way. */
	CHK(vmix.chanmap_nframes == ohrca.chanmap_nframes);
	CHK(vmix.chanmap_nframes > 1);

	/* 1. The marker exists, and carries the family. Neither build may emit the
	 *    other's form — a generator that ignored cfg would fail both arms. */
	int v_markers = count_marker_slots(&vmix, 0x00);
	int o_markers = count_marker_slots(&ohrca, 0x01);
	CHK(v_markers > 0);
	CHK(o_markers == v_markers);
	CHK(count_marker_slots(&vmix,  0x01) == 0);
	CHK(count_marker_slots(&ohrca, 0x00) == 0);

	/* 2. EXACTLY the marker byte moves. Walk every window byte-for-byte: the
	 *    only permitted differences are the marker's family byte and the block
	 *    checksum that covers it. */
	int diff_family = 0;
	for (int f = 0; f < vmix.chanmap_nframes; f++) {
		const uint8_t *a = vmix.chanmap[f], *b = ohrca.chanmap[f];
		for (int i = 0; i < 34; i++) {
			if (a[i] == b[i])
				continue;
			int is_marker_family = 0;
			for (int s = 0; s < 8; s++) {
				const uint8_t *t = SLOT(a, s);
				if (i == (7 + s * 3 + 1) && t[0] == 0xfe) {
					is_marker_family = 1;
					CHK(a[i] == 0x00 && b[i] == 0x01);
					diff_family++;
				}
			}
			/* [33] is the stamped block checksum: it MUST move with the byte. */
			if (!is_marker_family)
				CHK(i == 33);
		}
	}
	CHK(diff_family == v_markers);

	printf("ohrca chanmap: %d marker slots, %d family bytes flipped, rest identical\n",
	       v_markers, diff_family);
	return 0;
}
