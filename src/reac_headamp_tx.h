// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_headamp_tx — the MASTER-role head-amp SEND model (task #155 reac-pw half).
 *
 * A REAC console's preamp controls are DECLARATIVE, DMX-style: the desk does not
 * "toggle" 48V, it continuously RE-ASSERTS the absolute per-channel state (a live
 * M-200 broadcasts ~628 head-amp records per 14 grants, m200-headamp-re/DECODE.md).
 * There is no ACK; a single lost phantom-off record must not leave 48V on the pins,
 * so the master re-broadcasts the FULL table on a slow period and additionally
 * emits a record the instant a value changes (the edge).
 *
 * This module is the PURE state + scheduler for that: a per-wire-channel table of
 * (phantom, pad, sens) and a per-frame `next()` that says whether THIS slot should
 * carry a head-amp record and which one. It owns no socket and no frame buffer —
 * the caller (the pacer) turns a returned (ch, param, value) into wire bytes with
 * reac_ctrl_stamp_headamp over a FILLER slot. Keeping it separate from
 * reac_master's establishment FSM is deliberate: the head-amp overlay must never
 * alter or race the verified grant/chanmap/cfea emit path.
 *
 * OFF until an operator sets at least one cell (reac_headamp_tx_set); an all-unset
 * table's next() always returns 0, so the downstream is byte-identical to today.
 */
#ifndef REAC_HEADAMP_TX_H
#define REAC_HEADAMP_TX_H

#include <stdint.h>

#include <reac/reac.h>   /* REAC_MAX_CHANNELS */

/* The three head-amp params (indices into the per-channel row), aligned with
 * enum reac_headamp_param (PHANTOM=0, PAD=1, SENS=2). */
#define REAC_HEADAMP_NPARAMS 3

/* Re-assert the full table this often, expressed as a fraction of the frame rate:
 * period_frames = fps * NUM / DEN. ~1.5 s at any rate — conservative (a console
 * re-asserts far faster, but 1.5 s is well inside the "a dropped record must not
 * persist" budget while stealing a negligible number of the 8000/s FILLER slots).
 * A CHANGE is emitted immediately regardless of this period (the edge path). */
#define REAC_HEADAMP_REASSERT_NUM 3
#define REAC_HEADAMP_REASSERT_DEN 2

/* Slots between consecutive records DURING a full-table re-assert sweep. Matches
 * the grant burst's stride so the head-amp overlay never bunches records onto
 * back-to-back slots (one record per this many frames). */
#define REAC_HEADAMP_SWEEP_STRIDE 12

/* One operator-supplied head-amp cell, as carried from the CLI through the sink
 * + pacer config into the table (reac_headamp_tx_set). `ch` is the WIRE channel. */
struct reac_headamp_setting {
	uint8_t ch;
	uint8_t param;   /* reac_headamp_param */
	uint8_t value;
};

/* Pack/unpack a (ch, param, value) head-amp command into ONE 32-bit word.
 *
 * This is the atomicity primitive for the LIVE control path (task #203): a
 * controller's per-channel phantom/pad/sens change travels from the PipeWire
 * main-loop thread (the prop handler) to the RT pacer thread through a lock-free
 * SPSC queue of these words. Because the whole triple lives in a single 32-bit
 * cell, one atomic store/load carries it indivisibly — the RT reader can never
 * observe a ch from one command spliced onto the value of another (a torn
 * triple). ch (0..REAC_MAX_CHANNELS-1), param (0..2) and value (0..0x37) each
 * fit a byte, so the three pack losslessly into the low 24 bits. */
static inline uint32_t reac_headamp_pack(uint8_t ch, uint8_t param, uint8_t value)
{
	return (uint32_t)ch << 16 | (uint32_t)param << 8 | (uint32_t)value;
}

static inline void reac_headamp_unpack(uint32_t w, uint8_t *ch, uint8_t *param,
                                       uint8_t *value)
{
	*ch    = (uint8_t)((w >> 16) & 0xFF);
	*param = (uint8_t)((w >> 8) & 0xFF);
	*value = (uint8_t)(w & 0xFF);
}

struct reac_headamp_tx {
	uint8_t value[REAC_MAX_CHANNELS][REAC_HEADAMP_NPARAMS];
	uint8_t set[REAC_MAX_CHANNELS][REAC_HEADAMP_NPARAMS];   /* operator-assigned */
	uint8_t dirty[REAC_MAX_CHANNELS][REAC_HEADAMP_NPARAMS]; /* changed since emit */
	int     active;            /* >=1 cell set -> the sender is armed */

	int     reassert_period;   /* frames between full re-asserts (fps-scaled)   */
	int     reassert_tick;     /* counts down to the next full sweep            */
	int     sweeping;          /* mid full-table sweep                          */
	int     sweep_idx;         /* cursor over the flattened table during a sweep */
	int     sweep_wait;        /* frames to wait before the next sweep record   */
};

/* Initialize an empty (inactive) table; `fps` scales the re-assert period. */
void reac_headamp_tx_init(struct reac_headamp_tx *t, int fps);

/* Set one cell's absolute value and arm the sender. `ch` is the WIRE channel
 * (model_base + box_input-1, 0..REAC_MAX_CHANNELS-1); `param` is a
 * reac_headamp_param. Marks the cell dirty so the change is emitted on the next
 * eligible slot (the edge). Returns 0, or -1 on a bad ch/param/value. */
int reac_headamp_tx_set(struct reac_headamp_tx *t, uint8_t ch, uint8_t param,
                        uint8_t value);

/* Advance the scheduler by ONE frame and decide whether this slot carries a
 * head-amp record. Returns 1 and fills ch/param/value when it does, else 0.
 * Emits changed cells first (edge, one per call) and otherwise re-asserts the
 * whole table once per reassert_period, one record per REAC_HEADAMP_SWEEP_STRIDE
 * frames. An inactive table always returns 0. Call at most once per emitted
 * downstream frame (like reac_master_next). */
int reac_headamp_tx_next(struct reac_headamp_tx *t, uint8_t *ch, uint8_t *param,
                         uint8_t *value);

#endif /* REAC_HEADAMP_TX_H */
