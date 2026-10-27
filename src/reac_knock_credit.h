// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* THE KNOCK'S TRANSFER IS THE MASTER'S TRANSFER (#58).
 *
 * The knock pushes the very scene the master that follows pushes (reac_knock_wire.c,
 * master_at_header), and a box answers the complete push with its commit report and its
 * JOIN. That answer is handed to the master before its thread starts (reac_sink_cfg
 * .knock_answer), but a fresh master HOLDS a JOIN until a transfer of ITS OWN has gone
 * out whole (libreac reac_master.c hold_join: ~0.7 s at 8000 fps) and re-pushes the
 * scene meanwhile. Measured on the rig 2026-10-27, takes 5 and 6: the S-1608 answered the
 * knock with CONFIG + JOIN, was granted 0.69 s later by that release, and went silent
 * 1.2 s after ESTABLISHED, on every cycle. The build that keeps it grants on the JOIN
 * itself ("PROBING -> GRANTING (rx CONFIG)").
 *
 * So before the answer is ingested the master is credited the knock's delivery: it is
 * stepped once out of IDLE — reac_master_rx's own promotion runs enter_probing, which
 * zeroes the delivered count, so the credit has to follow a promotion and not precede
 * it — and then marked "one transfer delivered, none in flight" (the stepped slot is the
 * transfer's header, which a bare field write would otherwise leave in flight). The one
 * slot is never sent: a counter gap of one, which free-runs anyway. The carried JOIN
 * then takes the edge the moment it is ingested. */
#ifndef REAC_PW_KNOCK_CREDIT_H
#define REAC_PW_KNOCK_CREDIT_H

#include <reac/reac_master.h>

/* Credit `m` (fresh from reac_master_init, IDLE) with a whole scene transfer delivered
 * by the knock. Single-threaded by contract: before the pacer thread starts. */
void reac_knock_credit_transfer(struct reac_master *m);

#endif
