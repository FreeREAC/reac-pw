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
 *  - EACH CHANGE IS ONE RECORD. A lone op-0403 write of an absolute value
 *    self-commits (the LED follows); no commit pair exists on the wire.
 *  - A REAL M-200 THEN GOES SILENT. Captures hold 20.8-33 s of established
 *    traffic with phantom lit and zero head-amp records; the 1 Hz frame is the
 *    op-0103 CHANMAP heartbeat and carries no head-amp cell (its per-record
 *    byte1 is a bank marker). WE DO NOT COPY THAT. The protocol has no readback,
 *    so a master that asserts once has no mechanism that could ever discover a
 *    disagreement with the box, and one lost frame is one lost setting until the
 *    next establishment. Once ESTABLISHED, after REAC_HEADAMP_RESWEEP_SECONDS of
 *    head-amp silence, this re-emits the cells the operator SET — and only those,
 *    because the enrolling defaults below are safe-off phantom and re-sending
 *    them would darken a channel somebody lit at the box. The whole argument, the
 *    named risk and the rig gate are in docs/HEADAMP-REASSERT-POLICY.md.
 *
 * This module is the PURE state + scheduler for that: a per-wire-channel table of
 * (phantom, pad, sens), a REPLAY cursor armed complete on every entry into
 * ESTABLISHED and re-armed set-only on the re-assert cadence, and a per-frame
 * `next()` that says whether THIS slot should carry a head-amp record and which
 * one. It owns no socket and no frame buffer — the caller (the pacer) turns a
 * returned (ch, param, value) into wire bytes with reac_ctrl_stamp_headamp over a
 * FILLER slot. Keeping it separate from reac_master's establishment FSM is
 * deliberate: the head-amp overlay must never alter or race the verified
 * grant/chanmap/cfea emit path.
 */
#ifndef REAC_HEADAMP_TX_H
#define REAC_HEADAMP_TX_H

#include <stdint.h>

#include "reac_ctrl.h"   /* REAC_HEADAMP_MAX_CH, enum reac_headamp_param */

/* REAC_HEADAMP_NPARAMS (3, aligned with enum reac_headamp_param: PHANTOM=0,
 * PAD=1, SENS=2) comes from <reac/reac_ctrlblk.h> — it is a bound on the records
 * themselves, so it is declared once, with them. */

/* Slots between consecutive records DURING a complete-scene replay. Matches the
 * grant burst's stride so the head-amp overlay never bunches records onto
 * back-to-back slots (one record per this many frames): an 8-channel scene takes
 * ~36 ms at 8000 fps, a 32-channel one ~144 ms — inside the M-200's measured
 * 70-310 ms envelope. */
#define REAC_HEADAMP_SWEEP_STRIDE 12

/* HOW OFTEN THE SET CELLS ARE RE-ASSERTED, in seconds of head-amp silence.
 *
 * THIS IS A POLICY, not a protocol constant — the operator may change it, and
 * the two directions are both meaningful: raise it on a congested segment, lower
 * it on a rig that is losing records, set the period to 0 (see
 * reac_headamp_tx_set_resweep) and the behaviour is exactly the assert-once one a
 * real M-200 shows. The cost at 2 s is small enough that the number is not
 * delicate: records ride one per SWEEP_STRIDE frames on FILLER slots whose
 * control block is otherwise wasted, so a 16-input box with every cell set
 * spends ~72 ms of every 2 s at 8000 fps, about 0.3 % of slots, and a desk with
 * nothing set emits nothing at all. Reasoning and rig gate:
 * docs/HEADAMP-REASSERT-POLICY.md. */
#define REAC_HEADAMP_RESWEEP_SECONDS 2

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

	/* Scene replay cursor over base..base+width-1, all params per channel.
	 * width == 0 = idle. Armed COMPLETE on every entry into ESTABLISHED, and
	 * armed SET-ONLY by the re-assert cadence below. */
	uint8_t replay_base;
	uint8_t replay_width;
	int     replay_idx;        /* flattened cursor: ch_offset*NPARAMS + param */
	int     replay_wait;       /* frames until the next replay record */
	uint8_t replay_set_only;   /* this replay skips cells the operator never set */

	/* The slots the last COMPLETE scene was armed over, remembered so the
	 * re-assert can re-arm the same cursor without being told the box's geometry
	 * again — and so it stays silent until an establishment has armed one, which
	 * is what keeps REACPW_NO_HEADAMP a total silence rather than a delayed one. */
	uint8_t scene_base;
	uint8_t scene_width;

	/* The re-assert cadence, in FRAMES OF HEAD-AMP SILENCE (next() calls that
	 * emitted nothing). 0 = off, which is what init leaves it at: the pure module
	 * asserts once until a caller states a period. Counted from the last record
	 * ACTUALLY EMITTED rather than off a free-running clock, so an operator edge
	 * or a scene replay pushes the next sweep out and two sweeps can never
	 * overlap on the wire. */
	uint32_t resweep_period;
	uint32_t resweep_wait;
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

/* Set the periodic re-assert cadence in FRAMES, or 0 to disable it. The caller
 * (the pacer) converts REAC_HEADAMP_RESWEEP_SECONDS at the wire's frame rate,
 * which is also why it re-states the period after a rate change: 2 s is 16 000
 * frames at 8000 fps and 8 000 at 4000 fps. Re-stating restarts the silence
 * count. Single-writer: call on the pacer thread only (or before it starts). */
void reac_headamp_tx_set_resweep(struct reac_headamp_tx *t, uint32_t frames);

/* Advance the scheduler by ONE frame and decide whether this slot carries a
 * head-amp record. Returns 1 and fills ch/param/value when it does, else 0.
 * Emits changed cells first (edge, one per call), then walks a pending scene
 * replay one record per REAC_HEADAMP_SWEEP_STRIDE frames. Once both drain it
 * counts silent frames and, at the re-assert period, re-arms the replay over the
 * SET cells alone — never an unset cell's enrolling default. With no period set
 * it returns 0 forever after the drain. Call at most once per emitted downstream
 * frame (like reac_master_next). */
int reac_headamp_tx_next(struct reac_headamp_tx *t, uint8_t *ch, uint8_t *param,
                         uint8_t *value);

#endif /* REAC_HEADAMP_TX_H */
