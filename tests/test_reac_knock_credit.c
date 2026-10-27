// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_knock_credit — the master handed a knock's answer grants the JOIN it carried
 * at once (#58). Real frames through reac_pacer_rx_ingest, the seam reac_sink_node
 * uses before the pacer thread starts; no wire, no thread.
 *
 * THE DEFECT, rig 2026-10-27: the S-1608 answered the knock's complete scene push
 * with its commit report and JOIN; the fresh master held that JOIN for a push of its
 * own (~0.7 s), granted by that release, and the box went silent 1.2 s after
 * ESTABLISHED on every cycle. The box's answer belongs to a transfer already made. */
#include "reac_knock_credit.h"

#include <reac/transport/reac_pacer.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_master.h>
#include <reac/reac.h>

#include <stdio.h>
#include <string.h>
#include "reac_facts_pw.h"

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static const uint8_t OUR[6] = { 0x34, 0x5a, 0x60, 0x9f, 0x9e, 0xbe };
static const uint8_t BOX[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x3b };

static int fresh(struct reac_pacer *p)
{
	memset(p, 0, sizeof *p);
	p->handle = NULL;
	p->fps = REAC_PKT_RATE_96K;
	memcpy(p->src, OUR, 6);
	if (reac_frame_ring_init(&p->ring, REAC_BOX_S0808_IN, 2048) != 0)
		return -1;
	reac_master_init(&p->master, OUR, NULL, REAC_PKT_RATE_96K);
	p->prev_state = REAC_M_IDLE;
	return 0;
}

/* What the S-1608 answered the knock with, in the order the knock kept it: its
 * commit report (config announce), then its JOIN. */
static void answer(struct reac_pacer *p)
{
	uint8_t bf[2048];
	size_t bn;
	bn = reac_ctrl_build_config_announce(bf, OUR, BOX, 1, REAC_BOX_S1608_IN);
	reac_pacer_rx_ingest(p, bf, bn);
	bn = reac_ctrl_build_coldconnect(bf, OUR, BOX, 2, REAC_BOX_S1608_IN, NULL, REAC_SAMPLES_PER_PKT);
	reac_pacer_rx_ingest(p, bf, bn);
}

int main(void)
{
	struct reac_pacer p;

	/* ---- A. WITHOUT THE CREDIT, the answer is HELD: the master wants a push of its
	 * own first. This is the rig's 0.7 s, and the control for B. */
	CHK(fresh(&p) == 0);
	answer(&p);
	CHK(p.master.state == REAC_M_PROBING);
	CHK(p.master.join_held == 1);

	/* ---- B. WITH THE CREDIT, the answer takes the edge as it is ingested. */
	CHK(fresh(&p) == 0);
	reac_knock_credit_transfer(&p.master);
	CHK(p.master.state == REAC_M_PROBING);      /* promoted, and the credit survived it */
	CHK(p.master.scene_complete == 1);
	CHK(p.master.scene_inflight == 0);
	answer(&p);
	CHK(p.master.state == REAC_M_GRANTING);
	CHK(p.master.join_held == 0);
	CHK(reac_master_has_box(&p.master) == 1);

	/* ---- C. THE PROMOTION IS THE TRAP THE CREDIT HAS TO FOLLOW: a bare field write on
	 * an IDLE master is wiped by reac_master_rx's own IDLE -> PROBING. Kept as the
	 * reason the helper steps first (take 6 on the rig, 2026-10-27). */
	CHK(fresh(&p) == 0);
	p.master.scene_complete = 1;
	answer(&p);
	CHK(p.master.state == REAC_M_PROBING);
	CHK(p.master.join_held == 1);

	puts("ok: the master handed a knock's answer grants the JOIN it carried at once");
	return 0;
}
