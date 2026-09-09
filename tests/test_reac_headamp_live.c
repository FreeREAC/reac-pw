// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Live head-amp control handoff (task #203): the lock-free SPSC command ring that
 * carries a controller's per-channel phantom/pad/sens change from the PipeWire
 * main-loop thread (reac_pacer_headamp_set) to the RT pacer thread
 * (reac_pacer_headamp_drain -> reac_headamp_tx_set). No socket, no PipeWire, no RT
 * privilege: it drives the pacer's command ring + head-amp table directly, exactly
 * as the pacer thread does, so the emission logic and the concurrency contract are
 * both exercised offline.
 *
 *   1. Functional: a queued change is applied and emitted as a head-amp record.
 *   2. Concurrent-safe handoff: a single WRITER thread (the prop thread) floods the
 *      ring while this thread (the RT reader) drains + emits — and no torn
 *      (ch,param,value) is ever observed, because the triple is packed into one
 *      atomic word. The writer sends value == ch (SENS), so a spliced triple would
 *      surface as value != ch. */
#include "reac_pacer.h"
#include <reac/reac_headamp_tx.h>
#include <reac/reac_ctrl.h>
#include <reac/reac.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* Pack/unpack round-trip: the atomicity primitive the whole handoff rests on. */
static int test_pack(void)
{
	for (int ch = 0; ch < REAC_MAX_CHANNELS; ch++) {
		for (int p = 0; p < REAC_HEADAMP_NPARAMS; p++) {
			uint8_t v = (uint8_t)((ch * 7 + p * 13) & 0x37);
			uint32_t w = reac_headamp_pack((uint8_t)ch, (uint8_t)p, v);
			uint8_t rc, rp, rv;
			reac_headamp_unpack(w, &rc, &rp, &rv);
			if (rc != ch || rp != p || rv != v)
				return 0;
		}
	}
	return 1;
}

/* ---- concurrency stressor -------------------------------------------------- */

#define WRITER_CMDS 200000

struct writer_ctx {
	struct reac_pacer *p;
	_Atomic int done;
};

/* The PROP thread: floods the ring with (ch, SENS, value==ch) commands. It ONLY
 * produces (reac_pacer_headamp_set) — never touches the table or the tail. */
static void *writer(void *arg)
{
	struct writer_ctx *w = arg;
	for (long i = 0; i < WRITER_CMDS; i++) {
		uint8_t ch = (uint8_t)(i % REAC_MAX_CHANNELS);
		reac_pacer_headamp_set(w->p, ch, REAC_HEADAMP_SENS, ch);
	}
	atomic_store_explicit(&w->done, 1, memory_order_release);
	return NULL;
}

int main(void)
{
	CHK(test_pack());

	/* 1. FUNCTIONAL: a live change flows queue -> drain -> table -> emit. The pacer
	 * struct is zero-initialised (no socket opened); only the head-amp table +
	 * command ring are used. */
	{
		struct reac_pacer p;
		memset(&p, 0, sizeof p);
		reac_headamp_tx_init(&p.headamp);

		/* Nothing queued yet: the table is inactive and silent. */
		uint8_t ch, pr, v;
		CHK(reac_pacer_headamp_drain(&p) == 0);
		CHK(reac_headamp_tx_next(&p.headamp, &ch, &pr, &v) == 0);

		/* A controller sets phantom on ch 5. Enqueue (producer) then drain
		 * (consumer) — the table arms and emits the change as an edge. */
		CHK(reac_pacer_headamp_set(&p, 5, REAC_HEADAMP_PHANTOM, 1) == 1);
		CHK(reac_pacer_headamp_drain(&p) == 1);
		CHK(p.headamp.active == 1);
		CHK(reac_headamp_tx_next(&p.headamp, &ch, &pr, &v) == 1);
		CHK(ch == 5 && pr == REAC_HEADAMP_PHANTOM && v == 1);
		CHK(reac_headamp_tx_next(&p.headamp, &ch, &pr, &v) == 0);  /* edge consumed */

		/* A live change to the SAME cell mutates it and re-emits (no restart). */
		CHK(reac_pacer_headamp_set(&p, 5, REAC_HEADAMP_PHANTOM, 0) == 1);
		CHK(reac_pacer_headamp_drain(&p) == 1);
		CHK(reac_headamp_tx_next(&p.headamp, &ch, &pr, &v) == 1);
		CHK(ch == 5 && pr == REAC_HEADAMP_PHANTOM && v == 0);

		/* Multiple commands drained in one pass all reach the table. */
		CHK(reac_pacer_headamp_set(&p, 1, REAC_HEADAMP_SENS, 0x10) == 1);
		CHK(reac_pacer_headamp_set(&p, 2, REAC_HEADAMP_PAD, 1) == 1);
		CHK(reac_pacer_headamp_drain(&p) == 2);
		int got_sens = 0, got_pad = 0;
		for (int i = 0; i < 8; i++) {
			if (reac_headamp_tx_next(&p.headamp, &ch, &pr, &v)) {
				if (ch == 1 && pr == REAC_HEADAMP_SENS && v == 0x10) got_sens = 1;
				if (ch == 2 && pr == REAC_HEADAMP_PAD && v == 1) got_pad = 1;
			}
		}
		CHK(got_sens && got_pad);
	}

	/* 2. RING-FULL is benign: overflow drops the newest, never corrupts. Fill past
	 * capacity with the table never drained. */
	{
		struct reac_pacer p;
		memset(&p, 0, sizeof p);
		reac_headamp_tx_init(&p.headamp);
		int queued = 0;
		for (int i = 0; i < REAC_HEADAMP_CMD_RING + 32; i++)
			queued += reac_pacer_headamp_set(&p, 0, REAC_HEADAMP_SENS, 1);
		CHK(queued == REAC_HEADAMP_CMD_RING);              /* exactly capacity accepted */
		CHK(atomic_load(&p.ha_cmd_drops) == 32);           /* the rest dropped, counted */
		CHK(reac_pacer_headamp_drain(&p) == REAC_HEADAMP_CMD_RING);
	}

	/* 3. CONCURRENT-SAFE HANDOFF + NO TORN TRIPLE. One writer thread floods the ring
	 * while this thread drains + emits. The writer's invariant is value == ch (all
	 * SENS), so any emitted record with value != ch, or an out-of-range ch/param/
	 * value, would prove a torn read across the handoff. */
	{
		struct reac_pacer p;
		memset(&p, 0, sizeof p);
		reac_headamp_tx_init(&p.headamp);

		struct writer_ctx w = { .p = &p };
		atomic_store(&w.done, 0);

		pthread_t th;
		CHK(pthread_create(&th, NULL, writer, &w) == 0);

		uint64_t emitted = 0, applied = 0;
		uint8_t ch, pr, v;
		for (;;) {
			applied += (uint64_t)reac_pacer_headamp_drain(&p);
			/* Emit as fast as the table will yield — every record must be intact. */
			while (reac_headamp_tx_next(&p.headamp, &ch, &pr, &v)) {
				CHK(ch < REAC_MAX_CHANNELS);
				CHK(pr == REAC_HEADAMP_SENS);
				CHK(v <= REAC_HEADAMP_SENS_MAX);
				CHK(v == ch);            /* THE torn-triple assertion */
				emitted++;
			}
			if (atomic_load_explicit(&w.done, memory_order_acquire) &&
			    atomic_load(&p.ha_cmd_head) == atomic_load(&p.ha_cmd_tail))
				break;
		}
		CHK(pthread_join(th, NULL) == 0);

		/* Final sweep so no armed cell is left unverified. */
		reac_pacer_headamp_drain(&p);
		while (reac_headamp_tx_next(&p.headamp, &ch, &pr, &v)) {
			CHK(pr == REAC_HEADAMP_SENS && v == ch);
			emitted++;
		}

		/* We must have actually moved traffic (not a vacuous pass). */
		CHK(applied > 0);
		CHK(emitted > 0);
		fprintf(stderr, "  [concurrency] applied=%llu emitted=%llu drops=%llu\n",
		        (unsigned long long)applied, (unsigned long long)emitted,
		        (unsigned long long)atomic_load(&p.ha_cmd_drops));
	}

	printf("OK: live head-amp handoff — queue/drain/emit, ring-full drop, no torn triple\n");
	return 0;
}
