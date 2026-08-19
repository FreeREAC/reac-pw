// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_headamp_tx — the MASTER-role head-amp SEND model (task #155 reac-pw half).
 *
 * The protocol, as the committed captures show it (HEADAMP-PROTOCOL-AUDIT-2026-07-22
 * in reac-firmware-re; pcaps m200-s1608-BIDIR-reboot, matrix-m200-s1608 and
 * COLDCONNECT-clean in reac-captures):
 *
 *  - ARM ONCE, COMPLETELY, AT EVERY ESTABLISHMENT. A real M-200 puts the box's
 *    whole width x 3 head-amp scene on the wire ~10 ms behind the grant, one
 *    ~70-310 ms burst of per-channel param triples, with REAL values for every
 *    channel — a channel armed all-zero never enrols and ignores every later
 *    record. A box that POWER-CYCLES comes back with its pins blank and simply
 *    re-courts; the scene replay rides the new grant, unrequested and unACKed.
 *    That replay is the only mechanism that restores 48V after an outage.
 *  - THEN SILENCE. There is NO periodic re-assert on the wire — captures hold
 *    20.8-33 s of established traffic with phantom lit and zero head-amp
 *    records. The 1 Hz frame is the op-0103 CHANMAP heartbeat; it carries no
 *    head-amp cell (its per-record byte1 is a bank marker).
 *  - EACH CHANGE IS ONE RECORD. A lone op-0403 write of an absolute value
 *    self-commits (the LED follows); no commit pair exists on the wire.
 *
 * This module is the PURE state + scheduler for that: a per-wire-channel table of
 * (phantom, pad, sens), a one-shot complete-scene REPLAY cursor armed on every
 * entry into ESTABLISHED, and a per-frame `next()` that says whether THIS slot
 * should carry a head-amp record and which one. It owns no socket and no frame
 * buffer — the caller (the pacer) turns a returned (ch, param, value) into wire
 * bytes with reac_ctrl_stamp_headamp over a FILLER slot. Keeping it separate from
 * reac_master's establishment FSM is deliberate: the head-amp overlay must never
 * alter or race the verified grant/chanmap/cfea emit path.
 */
#ifndef REAC_HEADAMP_TX_H
#define REAC_HEADAMP_TX_H

#include <stdint.h>

#include "reac_ctrl.h"   /* REAC_HEADAMP_MAX_CH, enum reac_headamp_param */

/* The three head-amp params (indices into the per-channel row), aligned with
 * enum reac_headamp_param (PHANTOM=0, PAD=1, SENS=2). */
#define REAC_HEADAMP_NPARAMS 3

/* Slots between consecutive records DURING a complete-scene replay. Matches the
 * grant burst's stride so the head-amp overlay never bunches records onto
 * back-to-back slots (one record per this many frames): an 8-channel scene takes
 * ~36 ms at 8000 fps, a 32-channel one ~144 ms — inside the M-200's measured
 * 70-310 ms envelope. */
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
 * triple). ch (0..REAC_HEADAMP_MAX_CH-1), param (0..2) and value (0..0x37) each
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
	uint8_t value[REAC_HEADAMP_MAX_CH][REAC_HEADAMP_NPARAMS];
	uint8_t set[REAC_HEADAMP_MAX_CH][REAC_HEADAMP_NPARAMS];   /* operator-assigned */
	uint8_t dirty[REAC_HEADAMP_MAX_CH][REAC_HEADAMP_NPARAMS]; /* changed since emit */
	int     active;            /* >=1 cell set -> the edge sender is armed */

	/* One-shot complete-scene replay (armed on every entry into ESTABLISHED):
	 * cursor over base..base+width-1, all params per channel. width == 0 = idle. */
	uint8_t replay_base;
	uint8_t replay_width;
	int     replay_idx;        /* flattened cursor: ch_offset*NPARAMS + param */
	int     replay_wait;       /* frames until the next replay record */
};

/* Initialize an empty (inactive) table. */
void reac_headamp_tx_init(struct reac_headamp_tx *t);

/* Set one cell's absolute value and arm the sender. `ch` is the WIRE channel
 * (model_base + box_input-1, 0..REAC_HEADAMP_MAX_CH-1); `param` is a
 * reac_headamp_param. Marks the cell dirty so the change is emitted on the next
 * eligible slot (the edge). Returns 0, or -1 on a bad ch/param/value. */
int reac_headamp_tx_set(struct reac_headamp_tx *t, uint8_t ch, uint8_t param,
                        uint8_t value);

/* The enrolling DEFAULT a complete scene carries for an unset cell: phantom OFF
 * and pad OFF (the safe-off directions), SENS deliberately NON-ZERO — the box
 * only enrols a channel whose arming scene carries a real value. */
uint8_t reac_headamp_default(uint8_t param);

/* The value a complete scene carries for a cell: the table's if the operator set
 * it (a deliberate 0 counts — `set`, not the value, is the test), else the
 * enrolling default. NULL-safe: a NULL table is an all-default scene. */
uint8_t reac_headamp_tx_effective(const struct reac_headamp_tx *t,
                                  uint8_t ch, uint8_t param);

/* Arm the one-shot COMPLETE-SCENE replay over the granted box's head-amp slots
 * (base..base+width-1, clamped to the table). Call on EVERY entry into
 * ESTABLISHED — that is what a real M-200 does, and it is the only mechanism
 * that restores a power-cycled box's pins (48V included): the box reboots blank,
 * re-courts, and the scene rides the new establishment. Re-arming restarts the
 * cursor; width 0 is a no-op. Single-writer: call on the pacer thread only. */
void reac_headamp_tx_arm_scene(struct reac_headamp_tx *t, uint8_t base,
                               uint8_t width);

/* Advance the scheduler by ONE frame and decide whether this slot carries a
 * head-amp record. Returns 1 and fills ch/param/value when it does, else 0.
 * Emits changed cells first (edge, one per call), then walks a pending scene
 * replay one record per REAC_HEADAMP_SWEEP_STRIDE frames. With no edge pending
 * and no replay armed it returns 0 forever — committed state is held by the box.
 * Call at most once per emitted downstream frame (like reac_master_next). */
int reac_headamp_tx_next(struct reac_headamp_tx *t, uint8_t *ch, uint8_t *param,
                         uint8_t *value);

#endif /* REAC_HEADAMP_TX_H */
