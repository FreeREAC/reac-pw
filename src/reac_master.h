// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_master — the MASTER-role downstream control plane.
 *
 * reac_ctrl/reac_fsm are the SLAVE half (a virtual stagebox responding to a real
 * master). This module is the inverse: we ARE the REAC master so a real Roland
 * stagebox slaves to us. It drives the master's establishment state machine —
 * probe -> grant -> established + the periodic channel-map/announce — by writing
 * the real cdea/cfea control block into the 32-byte block [18:50] of the
 * downstream broadcast frame that reac_tx_build emits.
 *
 * EVENT-DRIVEN (task #130): a real M-5000 never advances the establishment on a
 * timer — it PROBES until the box's cold-connect (cdea 04 03) arrives, ECHOES
 * that block back as the grant burst, and settles the instant the box switches
 * to unicast. The earlier cut auto-advanced PROBING->GRANTING after ~1 s blind
 * (defect #130): it granted into silence and could reach ESTABLISHED with no
 * box on the wire. Now every forward transition is gated on a received box
 * control frame (reac_master_rx); timers remain only as SAFETY FALLBACKS that
 * move BACKWARD to PROBING (grant-window expiry, established peer-gone budget)
 * — never forward.
 *
 * The state machine is a PURE decision core (no I/O): the pacer thread owns it,
 * feeds it RX events (reac_master_rx) and asks it once per emitted frame what
 * control block to stamp (reac_master_next + reac_master_stamp). The cdea/cfea
 * cadence is carried in-band on the 8000 fps broadcast exactly as the real
 * master does (control frames occupy audio slots, never add to the stream).
 */
#ifndef REAC_MASTER_H
#define REAC_MASTER_H

#include <stdint.h>
#include <stddef.h>

/* The master establishment states (names stable; semantics per #130):
 *   IDLE        — pacer not emitting / PHY down / shutdown ONLY. The first
 *                 reac_master_next() call (= the pacer's first slot) enters
 *                 PROBING immediately and unconditionally: the golden evidence
 *                 shows a real unlinked M-5000 ALWAYS probes; there is no
 *                 observed silent-idle, and a master that waits for "presence"
 *                 deadlocks against a box whose PHY never bounced (§13b: the
 *                 box only cold-connects on a real link-down/up).
 *   PROBING     — unlinked hunting: FILLER + cdea 01 probes (~180/s, 00-heavy
 *                 sub-state cycle) + cfea announce @1 Hz. Leaves ONLY on a
 *                 validated box JOIN (REAC_M_RX_BOX_JOIN). No timer path out.
 *   GRANTING    — echo the box's own cdea 04 03 block back as the broadcast
 *                 grant burst (1 frame per 12 slots over a ~150 ms window).
 *                 -> ESTABLISHED on the box's first unicast-to-us frame of any
 *                 kind; window expiry with no unicast falls BACK to PROBING.
 *   ESTABLISHED — linked: FILLER audio + chanmap walk @1/s + cfea @1/s (two
 *                 independent streams, phase-offset by half a second). Held by
 *                 the 600-frame link-check budget reloaded by every box RX
 *                 event; BYE / budget-drain / box-MAC change drop it. */
enum reac_master_state {
	REAC_M_IDLE = 0,
	REAC_M_PROBING,
	REAC_M_GRANTING,
	REAC_M_ESTABLISHED,
};

/* Which control block the master stamps into the NEXT downstream frame. The
 * pacer maps this onto a block template (FILLER = leave the block zero and carry
 * audio; the cdea/cfea kinds overwrite [16:50]). */
enum reac_master_emit {
	REAC_M_EMIT_FILLER = 0, /* type 00 00, audio payload (the common case)      */
	REAC_M_EMIT_PROBE,      /* cdea 01, sub-state cycling (no box linked yet)   */
	REAC_M_EMIT_GRANT,      /* cdea 04 03 — the ECHO of the box's JOIN block    */
	REAC_M_EMIT_CHANMAP,    /* cdea 01 03 0019 established channel-map (1 of 6) */
	REAC_M_EMIT_ANNOUNCE,   /* cfea master announce (embeds OUR src MAC)        */
};

/* RX events the pacer feeds in (classified by reac_ctrl_classify_box_frame). */
enum reac_master_rx_event {
	REAC_M_RX_BOX_BCAST_FILLER = 0, /* box presence-flood (diagnostic only)     */
	REAC_M_RX_BOX_JOIN,             /* validated box cdea 04 03 cold-connect    */
	REAC_M_RX_BOX_UNICAST,          /* any unicast-to-us box frame (audio/hb/…) */
	REAC_M_RX_BOX_BYE,              /* box heartbeat with selector 0x00         */
};

/* Why the last backward transition happened (for the caller's logging). */
enum reac_master_drop_reason {
	REAC_M_DROP_NONE = 0,
	REAC_M_DROP_PEER_GONE,     /* established 600-frame budget drained          */
	REAC_M_DROP_BYE,           /* explicit box disconnect (hb selector 0x00)    */
	REAC_M_DROP_MAC_CHANGE,    /* JOIN from a different box — re-grant the new  */
	REAC_M_DROP_GRANT_TIMEOUT, /* grant window expired with no box unicast      */
};

/* Established link-check budget: the firmware 0x0258 = 600 frames (75 ms @96k /
 * 150 ms @48k), counted DOWN per emitted frame, RELOADED by every box RX event. */
#define REAC_M_LINKCHECK_RELOAD 600
/* Diagnostic presence flag decay (same frame budget as the link-check). */
#define REAC_M_PRESENCE_TIMEOUT 600
/* Grant burst density: one echoed grant per this many slots (~100 control
 * frames over the ~150 ms window @8000 fps, the transcribed real burst). */
#define REAC_M_GRANT_STRIDE 12

/* The probe sub-state cycle, 00-dominant per the §13d transcription (the real
 * unlinked master mostly advertises ss=00, touching 03/01/02 in the loop). */
extern const uint8_t REAC_M_PROBE_CYCLE[8];

/* How many distinct captured ESTABLISHED channel-map frames make a full walk. */
#define REAC_M_CHANMAP_FRAMES 6

struct reac_master {
	enum reac_master_state state;
	uint8_t  src[6];          /* our master MAC (Roland OUI) */
	uint16_t counter;         /* free-running u16-LE, +1 per emitted frame,
	                           * NEVER reset across transitions */

	int      fps;             /* frame rate (pps): 3675/4000/8000 */
	int      probe_period;    /* slots between probes (~fps/180 ≈ 180 probes/s) */
	int      grant_frames;    /* GRANTING window length (~150 ms of slots)      */
	int      grant_stride;    /* slots between echoed grants in the window      */

	/* PROBING cadence */
	int      probe_tick;      /* slots since the last probe */
	int      announce_tick;   /* slots since the last cfea (probing AND estab.) */
	int      probe_sub_idx;   /* cursor 0..7 into REAC_M_PROBE_CYCLE */

	/* GRANTING */
	int      grant_ticks;     /* slots elapsed in the current grant window */
	uint8_t  join_blk[32];    /* the box's cold-connect block — echoed verbatim */
	uint8_t  box_mac[6];      /* the joining box's L2 source */
	unsigned grant_attempts;  /* windows opened (diagnostic) */

	/* ESTABLISHED */
	int      chanmap_tick;    /* slots since the last chanmap frame */
	int      chanmap_cursor;  /* which of the 6 chanmap frames is next */
	int      link_check;      /* countdown to peer-gone (600-frame budget) */

	/* Diagnostics (never gate the establishment) */
	int      box_seen;        /* sustained box broadcast FILLER on the wire */
	int      presence_tick;   /* countdown to clearing box_seen */
	enum reac_master_drop_reason drop_reason;  /* last backward transition */

	/* Per-instance cfea announce: the captured template with OUR src MAC
	 * embedded (the capture's cloned 00:40:ab:ca:15:4d desk MAC was a
	 * documented slave-disconnect trigger — on-wire identity must match L2). */
	uint8_t  announce_blk[34];
};

/* Initialize for a given source MAC + frame rate (3675/4000/8000 fps). Starts
 * in IDLE; the first reac_master_next() call enters PROBING. */
void reac_master_init(struct reac_master *m, const uint8_t src[6], int fps);

/* Feed one classified RX event into the FSM (call from the FSM-owning thread
 * only). `box_src` is the frame's L2 source; `blk32` is the 32-byte control
 * block [18:50] and is required for JOIN (ignored otherwise, may be NULL).
 * Returns nonzero if a state transition happened (for the caller's logging;
 * inspect m->state / m->drop_reason for the details). */
int reac_master_rx(struct reac_master *m, enum reac_master_rx_event ev,
                   const uint8_t box_src[6], const uint8_t blk32[32]);

/* PURE: decide what the NEXT frame should carry, advancing the per-slot timers
 * by one frame. Returns the emit kind; *counter is set to the value to stamp at
 * bytes 14-15 (then internally incremented); *tmpl_idx is the template index to
 * pass to reac_master_stamp (chanmap frame 0..5 for CHANMAP, probe cycle index
 * 0..7 for PROBE; 0 otherwise). Call exactly once per emitted downstream frame.
 * Safety fallbacks (grant-window expiry, peer-gone budget) move the state
 * BACKWARD to PROBING here — no timer ever advances toward ESTABLISHED. */
enum reac_master_emit reac_master_next(struct reac_master *m, uint16_t *counter,
                                       int *tmpl_idx);

/* Stamp the control block for `emit` into a downstream frame already built by
 * reac_tx_build (1492 B: hdr + audio + C2 EA tail). For FILLER this is a no-op.
 * For the cdea/cfea kinds it overwrites type [16:18] + control block [18:50]
 * and applies the checksum, leaving audio + counter + tail intact. GRANT echoes
 * m->join_blk verbatim; ANNOUNCE uses m->announce_blk (our MAC embedded).
 * `tmpl_idx` selects the chanmap frame (0..5) or probe cycle slot (0..7).
 * Returns 0, or -1 on a bad kind/index. */
int reac_master_stamp(const struct reac_master *m, uint8_t *frame,
                      enum reac_master_emit emit, int tmpl_idx);

/* Human-readable names for the caller's logging. */
const char *reac_master_state_name(enum reac_master_state s);
const char *reac_master_rx_event_name(enum reac_master_rx_event e);
const char *reac_master_drop_name(enum reac_master_drop_reason r);

#endif /* REAC_MASTER_H */
