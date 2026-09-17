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
 * This process sets the tunable ONCE before anything reads it (libreac reads no
 * environment of its own — docs/design/specs/
 * 2026-09-17-tunables-api-and-shared-refusal-codes.md — reac_master_tunables_set()
 * is the daemon's own doorway), so it belongs in its own test binary rather than
 * test_reac_master's — a second call in the same process would just replace it.
 *
 * What must hold with the switch ON: EXACTLY the ENROLL frame disappears — the
 * grant burst's count, order and bytes, the dwell timing, and the ESTABLISHED
 * transition are all unaffected. Proving the surrounding sequence is untouched
 * is as important as proving the frame is gone: this is a subtraction, not a
 * redesign. test_reac_master's own establish() sequence (unset env) is the
 * byte-identical-default half of this proof. */
#include <reac/reac_master.h>
#include <reac/reac_ctrl.h>
#include <reac/reac_tunables.h>

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

/* The S-1608's head-amp base: the chassis strap the box ANNOUNCES (config
 * announce block[7]) times 0x10. It is a required argument now — the per-width
 * table it used to be inferred from is deleted, and a box that has not announced
 * is not a box. Same value the sibling master tests use. */
#define S1608_BASE 0x20

/* Stand in the quiet window between scene transfers, the way a box that joins
 * between two pushes does. A JOIN landing mid-push is HELD (reac_master_rx
 * returns 0) until the push finishes, so without this the JOIN below never opens
 * GRANTING. Mirrors test_reac_master's helper of the same name; the counter
 * free-runs one per slot, so the caller's tracker is resynced to it. */
static void deliver_scene(struct reac_master *m, uint16_t *expect_counter)
{
	uint16_t c;
	int ix;
	long guard = 0;
	while ((m->scene_complete == 0 || m->scene_inflight) &&
	       guard++ < 4L * m->cycle_len)
		(void)reac_master_next(m, &c, &ix);
	if (expect_counter)
		*expect_counter = m->counter;
}

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
	/* Set FIRST, before any reac_master_next() call. */
	struct reac_master_tunables mt = REAC_MASTER_TUNABLES_DEFAULT;
	mt.no_enroll = 1;
	reac_master_tunables_set(&mt);

	struct reac_master m;
	struct reac_console_cfg idle = REAC_CONSOLE_CFG_IDLE;
	uint16_t cnt = 0;
	reac_master_init(&m, SRC, &idle, FPS);

	/* The box declares itself FIRST — this is what reac_pacer does on the
	 * config-announce. There is no fabricated box to fall back to any more, so a
	 * JOIN arriving with no declaration in force has no enrollment to grant. */
	reac_master_set_box(&m, 16, 8, S1608_BASE);  /* S-1608: 16 in / 8 out */
	CHK(reac_master_has_box(&m) == 1);

	deliver_scene(&m, &cnt);
	CHK(reac_master_rx(&m, REAC_M_RX_BOX_JOIN, BOX, ZONEA_JOIN) == 1);
	CHK(m.state == REAC_M_GRANTING);
	CHK(m.grant_burst_len == 56);

	int grants = 0, last_grant_slot = -1, saw_enroll = 0;
	int idx;
	int span = m.grant_dwell + m.grant_burst_len * REAC_M_GRANT_STRIDE + 4;
	for (int i = 0; i < span; i++) {
		enum reac_master_emit e = slot(&m, &idx, &cnt);
		CHK(e != (enum reac_master_emit)-1);
		if (e == REAC_M_EMIT_ENROLL)
			saw_enroll = 1;
		/* No grant before the dwell ENDS — the real invariant, and the one that holds
		 * whether the dwell ended on the declaration or ran out its cap. */
		CHK(e != REAC_M_EMIT_GRANT || i > reac_master_grant_anchor(&m));
		if (e == REAC_M_EMIT_GRANT) {
			if (last_grant_slot >= 0)
				CHK(i - last_grant_slot == REAC_M_GRANT_STRIDE);
			else
				CHK(i == reac_master_grant_anchor(&m) + 1);
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
