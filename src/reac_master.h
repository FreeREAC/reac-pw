// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_master — the MASTER-role downstream control plane.
 *
 * reac_ctrl/reac_fsm are the SLAVE half (a virtual stagebox responding to a real
 * master). This module is the inverse: we ARE the REAC master so a real Roland
 * stagebox slaves to us. It drives the master's establishment state machine —
 * probe -> grant -> established + the periodic heartbeat/channel-map — by writing
 * the real cdea/cfea control block into the 32-byte block [18:50] of the
 * downstream broadcast frame that reac_tx_build emits.
 *
 * Why this exists: reac_tx_build zeroes the control block (type 00 00 = FILLER).
 * FILLER carries audio but is NOT a link grant, so no real desk locks to it. A
 * box only establishes when it sees the master cycle cdea 01 sub-states, then
 * GRANT with a cdea 04 03 burst, then settle to steady cdea 01 03 0019 channel-
 * map + cfea announce (REAC-PROTOCOL-AND-TESTS.md §13d, the gold WIRED reference).
 *
 * Byte source-of-truth: the control blocks here are the EXACT 32-byte blocks
 * captured off the real M-5000 master (reac-captures/wired-reac-a-bothdirs,
 * master 00:40:ab:ca:15:4d) — established channel-map (6 frames spanning the
 * full 40-ch walk), cfea announce, and the cdea probe/grant from the §6/§13d
 * transcription. Sum(block[18..49]) mod 256 == 0 holds on every one (verified).
 * We replay the captured block verbatim and re-stamp only the live counter at
 * [14:16]; nothing is reconstructed, so the bytes match the desk by construction.
 *
 * The state machine here is a PURE decision function (no I/O): given the master
 * state + the next frame's counter, it returns which control block to stamp into
 * the downstream frame. The pacer (reac_pacer) calls it once per emitted frame,
 * so the cdea/cfea cadence is carried in-band on the 8000 fps broadcast exactly
 * as the real master does (control frames occupy audio slots, never add to the
 * stream — see §9 reac-repacer note). The caller supplies the audio.
 */
#ifndef REAC_MASTER_H
#define REAC_MASTER_H

#include <stdint.h>
#include <stddef.h>

/* The master establishment states, mirroring the gold §13d sequence. */
enum reac_master_state {
	REAC_M_IDLE = 0,    /* PHY down / no box: emit nothing or plain FILLER */
	REAC_M_PROBING,     /* hunting: cycle cdea 01 sub-states 03->01->00->02 */
	REAC_M_GRANTING,    /* ~150 ms burst of cdea 04 03 (the connect grant)   */
	REAC_M_ESTABLISHED, /* linked: FILLER audio + steady cdea 01 03 + cfea ~1/s */
};

/* Which control block the master stamps into the NEXT downstream frame. The
 * pacer maps this onto a block template (FILLER = leave the block zero and carry
 * audio; the cdea/cfea kinds overwrite [16:50] with the captured block). */
enum reac_master_emit {
	REAC_M_EMIT_FILLER = 0, /* type 00 00, audio payload (the common case)      */
	REAC_M_EMIT_PROBE,      /* cdea 01, sub-state cycling (no box linked yet)   */
	REAC_M_EMIT_GRANT,      /* cdea 04 03 connect-grant burst                   */
	REAC_M_EMIT_CHANMAP,    /* cdea 01 03 0019 established channel-map (1 of 6) */
	REAC_M_EMIT_ANNOUNCE,   /* cfea master announce                             */
};

/* Establishment timing, frame-count gated (the real master's per-frame model).
 * Defaults are for 8000 fps (96 k); reac_master_init scales them to the rate. */
#define REAC_M_PROBE_FRAMES_DEFAULT   8000   /* ~1 s of probing before granting */
#define REAC_M_GRANT_FRAMES_DEFAULT   1200   /* ~150 ms grant burst @8000 fps   */
#define REAC_M_HEARTBEAT_PERIOD_DEF   8000   /* ~1 cdea/cfea per second          */

struct reac_master {
	enum reac_master_state state;
	uint8_t  src[6];          /* our master MAC (Roland OUI) */
	uint16_t counter;         /* free-running u16-LE, +1 per emitted frame */

	int      fps;             /* frame rate (pps): 3675/4000/8000 */
	int      probe_frames;    /* PROBING dwell before auto-grant */
	int      grant_frames;    /* GRANTING burst length */
	int      hb_period;       /* frames between control (cdea/cfea) frames */

	int      state_ticks;     /* frames elapsed in the current state */
	int      hb_tick;         /* frames since the last control frame */
	int      probe_sub_idx;   /* sub-state cursor 0..3 for the probe cycle */
	int      chanmap_cursor;  /* which of the N captured chanmap frames is next */
	int      announce_due;    /* alternate chanmap<->cfea on the control slot */
};

/* The probe sub-state cycle, in wire order (03 dominates briefly, then 00). */
extern const uint8_t REAC_M_PROBE_SUBSTATES[4];

/* How many distinct captured ESTABLISHED channel-map frames make a full walk. */
#define REAC_M_CHANMAP_FRAMES 6

/* Initialize for a given source MAC + frame rate (3675/4000/8000 fps). */
void reac_master_init(struct reac_master *m, const uint8_t src[6], int fps);

/* Tell the master a box is present / absent (PHY up/down, or RX presence-flood
 * seen). present=1 with state IDLE -> PROBING; present=0 -> IDLE (drop). */
void reac_master_set_box_present(struct reac_master *m, int present);

/* PURE: decide what the NEXT frame should carry, advancing the state machine by
 * one frame. Returns the emit kind; *counter is set to the value to stamp at
 * bytes 14-15 of that frame (then internally incremented); *tmpl_idx is the
 * template index to pass to reac_master_stamp (the channel-map frame 0..5 for
 * CHANMAP, or the probe sub-state index 0..3 for PROBE; 0 otherwise). Call
 * exactly once per emitted downstream frame. */
enum reac_master_emit reac_master_next(struct reac_master *m, uint16_t *counter,
                                       int *tmpl_idx);

/* Stamp the control block for `emit` into a downstream frame already built by
 * reac_tx_build (1492 B: hdr + audio + C2 EA tail). For FILLER this is a no-op
 * (the zero block + audio that reac_tx_build wrote is correct). For the cdea/cfea
 * kinds it overwrites type [16:18] + control block [18:50] with the captured
 * master block and applies the checksum, leaving the audio + counter + tail
 * intact. `chanmap_idx` selects which of the 6 channel-map frames (0..5) for
 * REAC_M_EMIT_CHANMAP; ignored otherwise. Returns 0, or -1 on a bad kind. */
int reac_master_stamp(uint8_t *frame, enum reac_master_emit emit, int chanmap_idx);

#endif /* REAC_MASTER_H */
