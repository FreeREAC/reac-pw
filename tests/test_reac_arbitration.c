// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_arbitration — who drives this segment, computed from sightings + our own FSM.
 *
 * The incident this comes from: an S-4000S in SLAVE mode produced a sighting of role `master`,
 * a model that never resolved, and a reac-pw that probed forever. The box's own cold-connect
 * frames classify through a catch-all, and nothing added the evidence up into a statement about
 * the segment. So the tests below are mostly about what must NOT become a master.
 */
#include "reac_arbitration.h"
#include <reac/reac.h>   /* the geometry that outranks the control plane */

#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

#define SEC 1000000000ULL

static const uint8_t OURS[6]  = { 0x34, 0x5a, 0x60, 0x9f, 0x9e, 0xbe };  /* this rig's NIC */
static const uint8_t DESK[6]  = { 0x00, 0x40, 0xab, 0xc9, 0xcc, 0x03 };  /* a real M-200 */
static const uint8_t BOX[6]   = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x41 };  /* the S-1608 */

static void put(struct reac_disco_table *t, const uint8_t mac[6],
                enum reac_disco_role role, uint64_t seen_ns)
{
	struct reac_disco_entry *e = &t->e[t->n++];
	memset(e, 0, sizeof *e);
	memcpy(e->mac, mac, 6);
	e->role = role;
	e->first_seen_ns = seen_ns;
	e->last_seen_ns = seen_ns;
}

int main(void)
{
	struct reac_arbitration a;
	struct reac_disco_table t;
	const uint64_t now = 100 * SEC;

	/* 1. A SILENT WIRE IS NOT US. Nothing established, nothing probing, no evidence —
	 * reported as NONE with no MAC. "We drive" is a claim, and an idle daemon has not
	 * earned it. */
	reac_disco_table_init(&t);
	reac_arbitrate(&t, OURS, REAC_M_IDLE, REAC_PACE_FREE_RUN, now, &a);
	CHK(a.state == REAC_SEGMENT_NONE);
	CHK(a.have_mac == 0);
	CHK(strcmp(reac_segment_master_name(a.state), "none") == 0);

	/* 2. PROBING UNOPPOSED IS US: the segment is ours to take, and the MAC we publish is
	 * our own. */
	reac_disco_table_init(&t);
	reac_arbitrate(&t, OURS, REAC_M_PROBING, REAC_PACE_FREE_RUN, now, &a);
	CHK(a.state == REAC_SEGMENT_US);
	CHK(a.have_mac == 1 && memcmp(a.mac, OURS, 6) == 0);

	/* 3. A BOX IS NOT A MASTER. A box sighting, however loud, never makes the segment
	 * foreign — this is the S-4000S incident's shape. */
	reac_disco_table_init(&t);
	put(&t, BOX, REAC_DISCO_ROLE_BOX, now);
	reac_arbitrate(&t, OURS, REAC_M_PROBING, REAC_PACE_FREE_RUN, now, &a);
	CHK(a.state == REAC_SEGMENT_US);

	/* 4. AN UNKNOWN-ROLE SIGHTING IS NOT EVIDENCE EITHER WAY. It does not become a master,
	 * and it does not contradict one. The catch-all bucket is exactly where a box gets
	 * misfiled, and the cost of acting on that misfile is a dead segment. */
	reac_disco_table_init(&t);
	put(&t, DESK, REAC_DISCO_ROLE_UNKNOWN, now);
	reac_arbitrate(&t, OURS, REAC_M_PROBING, REAC_PACE_FREE_RUN, now, &a);
	CHK(a.state == REAC_SEGMENT_US);
	CHK(a.pace == REAC_PACE_FREE_RUN);   /* and it does not take the pace either */

	/* 5. AN UNAMBIGUOUS FOREIGN MASTER TAKES THE SEGMENT — and the pace with it. One master
	 * per segment is REAC law and we are not the second one. */
	reac_disco_table_init(&t);
	put(&t, DESK, REAC_DISCO_ROLE_MASTER, now);
	reac_arbitrate(&t, OURS, REAC_M_PROBING, REAC_PACE_FREE_RUN, now, &a);
	CHK(a.state == REAC_SEGMENT_FOREIGN);
	CHK(a.have_mac == 1 && memcmp(a.mac, DESK, 6) == 0);
	CHK(a.pace == REAC_PACE_FOREIGN_MASTER);
	CHK(strcmp(reac_pace_source_name(a.pace), "foreign-master") == 0);

	/* 6. OUR OWN ECHO IS NOT A RIVAL. Two masters sourcing the same real MAC each discarding
	 * the other as own-echo is a measured trap on this rig; classifying our own frames as a
	 * foreign master would be the same mistake wearing the other hat. */
	reac_disco_table_init(&t);
	put(&t, OURS, REAC_DISCO_ROLE_MASTER, now);
	reac_arbitrate(&t, OURS, REAC_M_PROBING, REAC_PACE_FREE_RUN, now, &a);
	CHK(a.state == REAC_SEGMENT_US);

	/* 7. A MASTER HEARD ONCE AND GONE IS NOT A MASTER. An unplugged desk must stop being
	 * reported, or a segment stays permanently "foreign" to a machine that left. */
	reac_disco_table_init(&t);
	put(&t, DESK, REAC_DISCO_ROLE_MASTER, now - (REAC_DISCO_STALE_NS + SEC));
	reac_arbitrate(&t, OURS, REAC_M_PROBING, REAC_PACE_FREE_RUN, now, &a);
	CHK(a.state == REAC_SEGMENT_US);

	/* 8. ESTABLISHED MEANS WE DRIVE, and a rival appearing now is the MID-FLIGHT CONFLICT:
	 * reported, not acted on. Saying `foreign` would tell a surface the desk had taken over
	 * when the audio is still ours; saying nothing would hide a broken segment. Yield-or-hold
	 * is the operator's call (spec §6 Q1) and neither is done here. */
	reac_disco_table_init(&t);
	put(&t, DESK, REAC_DISCO_ROLE_MASTER, now);
	reac_arbitrate(&t, OURS, REAC_M_ESTABLISHED, REAC_PACE_FREE_RUN, now, &a);
	CHK(a.state == REAC_SEGMENT_US);
	CHK(a.conflict == 1);
	CHK(memcmp(a.mac, OURS, 6) == 0);     /* the driver is still us */

	/* ... and with no rival, established is simply us, with nothing to report. */
	reac_disco_table_init(&t);
	put(&t, BOX, REAC_DISCO_ROLE_BOX, now);
	reac_arbitrate(&t, OURS, REAC_M_ESTABLISHED, REAC_PACE_FREE_RUN, now, &a);
	CHK(a.state == REAC_SEGMENT_US);
	CHK(a.conflict == 0);

	/* 9. THE NEWEST RIVAL IS THE ONE NAMED. Two foreign masters is already a broken segment;
	 * naming the one still talking beats naming whichever was recorded first. */
	reac_disco_table_init(&t);
	put(&t, DESK, REAC_DISCO_ROLE_MASTER, now - (2 * SEC));
	static const uint8_t OTHER[6] = { 0x00, 0x40, 0xab, 0xca, 0x15, 0x4c };
	put(&t, OTHER, REAC_DISCO_ROLE_MASTER, now);
	reac_arbitrate(&t, OURS, REAC_M_PROBING, REAC_PACE_FREE_RUN, now, &a);
	CHK(a.state == REAC_SEGMENT_FOREIGN);
	CHK(memcmp(a.mac, OTHER, 6) == 0);

	/* 10. OUR OWN PACE REFERENCE IS CARRIED THROUGH when we drive — the arbitration does not
	 * invent it, it reports whichever the clock election settled on. */
	reac_disco_table_init(&t);
	reac_arbitrate(&t, OURS, REAC_M_ESTABLISHED, REAC_PACE_GRAPH_REF, now, &a);
	CHK(a.pace == REAC_PACE_GRAPH_REF);
	CHK(strcmp(reac_pace_source_name(a.pace), "graph-ref") == 0);

	/* THE ROLE IS THE GEOMETRY. A peer that classifies MASTER while emitting a box
	 * width is a stagebox strapped to master mode, and a master never joins another
	 * master — so arbitration reads the length, not only the control plane. The
	 * widths are the chassis we own; 1494 carries the FCS residue and is no geometry. */
	CHK(reac_frame_is_master_downstream(REAC_FRAME_BYTES));
	CHK(!reac_frame_is_master_downstream(1204));
	CHK(!reac_frame_is_master_downstream(628));
	CHK(!reac_frame_is_master_downstream(340));
	CHK(!reac_frame_is_master_downstream(REAC_FRAME_BYTES_OHRCA));
	CHK(reac_frame_channels(1204) == 32);   /* S-4000S */
	CHK(reac_frame_channels(628) == 16);    /* S-1608  */
	CHK(reac_frame_channels(340) == 8);     /* S-0808  */
	CHK(reac_frame_channels(REAC_FRAME_BYTES) == 40);
	CHK(reac_frame_channels(REAC_FRAME_BYTES_OHRCA) == 0);

	printf("test_reac_arbitration: OK\n");
	return 0;
}
