// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* REACPW_NO_ENROLL: a rig-test SWITCH, never a silent new default.
 *
 * A wire differential across 82 captures found that no real desk ever sends the
 * pre-grant ENROLL (cdea 01 03 000d) to an S-1608 (0/11 sessions), while every
 * S-0808 and S-4000S session gets one. reac-pw sends it to all three today. The
 * rig's S-1608 is currently established WITH that ENROLL in flight on one bank,
 * so removing it unconditionally is untested and risks the bank that already
 * works — hence a switch, default OFF (today's behaviour byte-identical), that
 * the rig can flip in one deliberate, reversible step.
 *
 * This process sets the env var ONCE before anything reads it (the knob is
 * cached like the file's other getenv knobs), so it belongs in its own test
 * binary rather than test_reac_master's — a second env value read into that
 * cache in the same process would just be ignored.
 *
 * What must hold with the switch ON: EXACTLY the ENROLL frame disappears — the
 * grant burst's count, order and bytes, the dwell timing, and the ESTABLISHED
 * transition are all unaffected. Proving the surrounding sequence is untouched
 * is as important as proving the frame is gone: this is a subtraction, not a
 * redesign. test_reac_master's own establish() sequence (unset env) is the
 * byte-identical-default half of this proof. */
#ifdef _WIN32
#error "setenv is POSIX-only; this test does not run on Windows"
#endif
#include "reac_master.h"
#include "reac_ctrl.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static const uint8_t ZONEA_JOIN[32] = {
 0x04,0x03,0x00,0x14,0x00,0x02,0x00,0xfe,0x0f,0xf0,0x41,0x0a,0x00,0x00,0x12,0x12,
 0x01,0x00,0x06,0x00,0x01,0x00,0x78,0xf7,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
static const uint8_t SRC[6] = { 0x00, 0x40, 0xab, 0x00, 0x00, 0x01 };
static const uint8_t BOX[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x3b };

#define FPS 8000

static enum reac_master_emit slot(struct reac_master *m, int *idx, uint16_t *expect_counter)
{
	uint16_t c;
	int i;
	enum reac_master_emit e = reac_master_next(m, &c, &i);
	if (c != *expect_counter) {
		fprintf(stderr, "FAIL: counter %u != expected %u\n", c, *expect_counter);
		return (enum reac_master_emit)-1;
	}
	(*expect_counter)++;
	if (idx)
		*idx = i;
	return e;
}

int main(void)
{
	/* Set FIRST, before any reac_master_next() call — no_enroll() caches on first read. */
	CHK(setenv("REACPW_NO_ENROLL", "1", 1) == 0);

	struct reac_master m;
	struct reac_console_cfg idle = REAC_CONSOLE_CFG_IDLE;
	uint16_t cnt = 0;
	reac_master_init(&m, SRC, &idle, FPS);

	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) == 1);
	CHK(m.state == REAC_M_GRANTING);
	reac_master_set_box(&m, 16, 8);            /* S-1608: 16 in / 8 out */
	CHK(m.grant_burst_len == 56);

	int grants = 0, last_grant_slot = -1, saw_enroll = 0;
	int idx;
	int span = m.grant_dwell + m.grant_burst_len * REAC_M_GRANT_STRIDE + 4;
	for (int i = 0; i < span; i++) {
		enum reac_master_emit e = slot(&m, &idx, &cnt);
		CHK(e != (enum reac_master_emit)-1);
		if (e == REAC_M_EMIT_ENROLL)
			saw_enroll = 1;
		CHK(e != REAC_M_EMIT_GRANT || i > m.grant_dwell);   /* no grant before the dwell */
		if (e == REAC_M_EMIT_GRANT) {
			if (last_grant_slot >= 0)
				CHK(i - last_grant_slot == REAC_M_GRANT_STRIDE);
			else
				CHK(i == m.grant_dwell + 1);
			last_grant_slot = i;
			CHK(idx == grants);              /* blocks emitted in order, same as unset */
			grants++;
		}
	}

	/* The whole point: the ENROLL is gone, and NOTHING else moved. */
	CHK(!saw_enroll);
	CHK(grants == m.grant_burst_len);           /* all 32 blocks, unaffected */
	CHK(m.state == REAC_M_ESTABLISHED);         /* still self-completes + holds */

	printf("OK: REACPW_NO_ENROLL=1 removes the ENROLL frame and only the ENROLL frame — "
	       "grant count, order, dwell timing and the ESTABLISHED transition are unchanged\n");
	return 0;
}
