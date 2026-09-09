// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test for reac_link_state_from_master / reac_link_state_name — the pure
 * mapping the reac.link-state PipeWire property (task #154's stagebox-badge
 * need) is stamped from. Pins:
 *   (a) every reac_master_state maps to its badge string, with IDLE folded
 *       into "probing" alongside PROBING itself (#130: a real master never
 *       observably idles);
 *   (b) the just_dropped overlay always wins, regardless of `st`;
 *   (c) the exact wire strings a consumer (openmixer's stagebox card) matches
 *       on are stable ("probing" / "granting" / "established" / "dropped").
 */
#include <reac/reac_link_state.h>

#include <stdio.h>
#include <string.h>

#define CHK(cond) do { if (!(cond)) { \
	fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__); return 1; } } while (0)

int main(void)
{
	/* ---- (a) each FSM state maps to its badge value, not dropped. */
	CHK(reac_link_state_from_master(REAC_M_IDLE, 0) == REAC_LINK_PROBING);
	CHK(reac_link_state_from_master(REAC_M_PROBING, 0) == REAC_LINK_PROBING);
	CHK(reac_link_state_from_master(REAC_M_GRANTING, 0) == REAC_LINK_GRANTING);
	CHK(reac_link_state_from_master(REAC_M_ESTABLISHED, 0) == REAC_LINK_ESTABLISHED);

	/* ---- (b) the drop overlay wins over every state, including a state that
	 * would otherwise read as good link (ESTABLISHED/GRANTING) — the caller
	 * only ever sets it true on the exact drain cycle the backward transition
	 * fired, but the mapping itself must not second-guess that. */
	CHK(reac_link_state_from_master(REAC_M_IDLE, 1) == REAC_LINK_DROPPED);
	CHK(reac_link_state_from_master(REAC_M_PROBING, 1) == REAC_LINK_DROPPED);
	CHK(reac_link_state_from_master(REAC_M_GRANTING, 1) == REAC_LINK_DROPPED);
	CHK(reac_link_state_from_master(REAC_M_ESTABLISHED, 1) == REAC_LINK_DROPPED);

	/* ---- (c) the exact wire strings the badge consumer matches on. */
	CHK(strcmp(reac_link_state_name(REAC_LINK_PROBING), "probing") == 0);
	CHK(strcmp(reac_link_state_name(REAC_LINK_GRANTING), "granting") == 0);
	CHK(strcmp(reac_link_state_name(REAC_LINK_ESTABLISHED), "established") == 0);
	CHK(strcmp(reac_link_state_name(REAC_LINK_DROPPED), "dropped") == 0);

	printf("OK: link-state mapping — every FSM state -> its badge string, "
	       "drop overlay wins, wire strings stable\n");
	return 0;
}
