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
 *   PROBING     — unlinked: FILLER + the continuous M-300 control cadence
 *                 (PROBE ~115/s + sub01/sub02/chanmap/cfea @1 Hz each). The
 *                 chanmap advertises the sub-state-0x03 map the box's parser
 *                 needs to recognize a master. Leaves ONLY on a validated box
 *                 JOIN (REAC_M_RX_BOX_JOIN). No timer path out.
 *   GRANTING    — echo the box's own cdea 04 03 block back as the broadcast
 *                 grant burst (1 frame per 12 slots over a ~150 ms window).
 *                 -> ESTABLISHED on the box's first unicast-to-us frame of any
 *                 kind; window expiry with no unicast falls BACK to PROBING.
 *   ESTABLISHED — linked: FILLER audio + the SAME continuous control cadence as
 *                 PROBING (PROBE + sub01/sub02/chanmap/cfea). Held by the
 *                 600-frame link-check budget reloaded by every box RX event;
 *                 BYE / budget-drain / box-MAC change drop it. */
enum reac_master_state {
	REAC_M_IDLE = 0,
	REAC_M_PROBING,
	REAC_M_GRANTING,
	REAC_M_ESTABLISHED,
};

/* Which control block the master stamps into the NEXT downstream frame. The
 * pacer maps this onto a block template (FILLER = leave the block zero and carry
 * audio; the cdea/cfea kinds overwrite [16:50]). The five control messages a
 * real master advertises CONTINUOUSLY (in both unlinked and linked states, per
 * the byte-exact M-300/S-1608 capture) are PROBE (~115/s) + SUB01/SUB02/CHANMAP/
 * CFEA (~1/s each). GRANT is the only event-driven emission (fires on a JOIN). */
enum reac_master_emit {
	REAC_M_EMIT_FILLER = 0, /* type 00 00, audio payload (the common case)      */
	REAC_M_EMIT_PROBE,      /* cdea 01 00 — the fixed M-300 probe (~115/s)      */
	REAC_M_EMIT_SUB01,      /* cdea 01 01 — the fixed M-300 sub-message (~1/s)  */
	REAC_M_EMIT_SUB02,      /* cdea 01 02 — the fixed M-300 sub-message (~1/s)  */
	REAC_M_EMIT_GRANT,      /* cdea 04 03 — the ECHO of the box's JOIN block    */
	REAC_M_EMIT_CHANMAP,    /* cdea 01 03 0019 generated channel-map (1 of N)   */
	REAC_M_EMIT_ANNOUNCE,   /* cfea master announce (generated: OUR MAC + I/O)  */
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

/* The REAC fabric is a RING of 49 positions: channels 0x00..0x2f (48) followed by
 * the 0xfe section marker at the wrap. A channel-map frame advertises 8 consecutive
 * ring positions, and a real master emits ONE window per start position — so the
 * full sweep is exactly 49 frames (measured live off an M-200 driving an S-1608,
 * 2026-07-11, #130). An earlier 11-frame figure came from an M-300 capture too
 * short to contain the whole rotation. */
#define REAC_M_FABRIC_RING        49
#define REAC_M_CHANMAP_FRAMES_MAX REAC_M_FABRIC_RING

/* Console I/O config: everything the downstream generator needs to synthesize
 * the chanmap + cfea for a specific box. The master MAC is NOT here — it is OUR
 * L2 source MAC (passed to reac_master_init), which the cfea embeds so the
 * on-wire announced identity always equals the L2 source (a mismatch is a
 * documented slave-disconnect trigger). Fed the S-1608 config the generator
 * reproduces the captured M-300 downstream byte-for-byte. */
struct reac_console_cfg {
	uint8_t out_channels;   /* box analog outputs: cfea outCh [18]. (Does NOT
	                         * size the chanmap: a real master sweeps the whole
	                         * 40-slot fabric regardless of console width, #130.)
	                         * S-1608 = 8, M-5000 downstream box = 16.        */
	uint8_t in_channels;    /* box analog inputs: sizes the UPSTREAM parser
	                         * (box->master); carried for the caller, not a
	                         * downstream field. S-1608 = 16.                 */
	uint8_t console_field;  /* cfea [19] and [21] (move together): the emulated
	                         * MASTER model. M-300 = 0x00, M-5000 = 0x01.     */
};

/* The default (S-1608 driven by an emulated M-300): 8 out, 16 in, console 0.
 * reac_master_init(cfg == NULL) uses this, so the byte-exact S-1608 path is the
 * out-of-the-box behaviour. */
#define REAC_CONSOLE_CFG_S1608 \
	((struct reac_console_cfg){ .out_channels = 8, .in_channels = 16, .console_field = 0 })

struct reac_master {
	enum reac_master_state state;
	uint8_t  src[6];          /* our master MAC (Roland OUI) */
	uint16_t counter;         /* free-running u16-LE, +1 per emitted frame,
	                           * NEVER reset across transitions */

	int      fps;             /* frame rate (pps): 3675/4000/8000 */
	int      grant_frames;    /* GRANTING window length (~150 ms of slots)      */
	int      grant_stride;    /* slots between echoed grants in the window      */

	/* The console we advertise + the downstream blocks generated from it. */
	struct reac_console_cfg cfg;
	int      chanmap_nframes; /* generated chanmap frame count (>=1)            */
	uint8_t  chanmap[REAC_M_CHANMAP_FRAMES_MAX][34]; /* generated cdea chanmap  */

	/* CYCLE-LOCKED control cadence (#130, measured off the M-300/S-1608 establish
	 * capture; identical in PROBING and ESTABLISHED). A real master's control
	 * plane is one deterministic cycle of `cycle_len` slots (10778 @ 4000 fps =
	 * 2.69 s, scaled by fps):
	 *   - a probe BURST: one probe every `probe_stride` slots (8 @ 4000 fps =
	 *     500/s) from slot 0 through `burst_end` (341 probes), probe indices
	 *     30..33 being the 4 inventory specials (zeros / our-MAC / SYSP / SCEN);
	 *   - a probe-free PAUSE for the rest of the cycle, holding sub02 right
	 *     after the burst, ONE chanmap window mid-pause (the 49-window sweep
	 *     thus takes 49 cycles), and sub01 at the cycle's tail.
	 * The old model ("PROBE ~115/s uniform + everything at 1/s") was the duty-
	 * cycle AVERAGE of this rhythm — a capture-analysis artifact; a box never
	 * sees a real master emit that way. cfea free-runs at ~1/s (measured),
	 * independent of the cycle. */
	int      cycle_len;       /* slots per control cycle (fps*10778/4000)       */
	int      cycle_pos;       /* current slot in the cycle [0, cycle_len)       */
	int      probe_stride;    /* slots between burst probes (fps/500)           */
	int      burst_end;       /* last probe slot: (341-1)*probe_stride          */
	int      sub02_off;       /* cdea 01 02 slot: burst_end + probe_stride      */
	int      chanmap_off;     /* chanmap slot: fps*5953/4000 (mid-pause)        */
	int      sub01_off;       /* cdea 01 01 slot: cycle_len - 5                 */
	int      probe_idx;       /* burst probe index (cycle_pos/stride) of the
	                           * probe being emitted (set by the cadence)       */
	int      announce_tick;   /* slots since the last cfea (~1/s, free-running) */

	/* PROBE ROTATION (#130, measured live off an M-200 2026-07-11). The probe is
	 * NOT a fixed constant: its 27-byte payload is a sliding window over the
	 * period-10 sequence [00 00 00 01 00 00 00 00 00 SUB], where SUB = 0x02 while
	 * hunting and 0x03 once established. The phase advances +6 (mod 10) after every
	 * 2 emissions -> the observed 0,6,2,8,4 rotation. EVERY FILLER frame's [18:50]
	 * descriptor is 16x "00 <cksum-of-the-current-probe>" — the descriptor tracks
	 * the probe, which is why a real master's FILLER descriptor appears to "cycle".
	 * A frozen probe (and hence a frozen descriptor) is what left real boxes mute. */
	int      probe_phase;     /* current phase into the period-10 sequence      */
	int      probe_repeat;    /* emissions done at this phase (2 per phase)     */
	uint8_t  probe_blk[34];   /* the current probe [type|block], regenerated    */
	uint8_t  filler_desc;     /* = current probe's checksum; stamped into FILLER */

	/* GRANTING */
	int      grant_ticks;     /* slots elapsed in the current grant window */
	uint8_t  join_blk[32];    /* the box's cold-connect block — echoed verbatim */
	uint8_t  box_mac[6];      /* the joining box's L2 source */
	unsigned grant_attempts;  /* windows opened (diagnostic) */

	/* ESTABLISHED */
	int      chanmap_cursor;  /* which generated chanmap frame is next (0..N-1) */
	int      link_check;      /* countdown to peer-gone (600-frame budget) */

	/* Diagnostics (never gate the establishment) */
	int      box_seen;        /* sustained box broadcast FILLER on the wire */
	int      presence_tick;   /* countdown to clearing box_seen */
	enum reac_master_drop_reason drop_reason;  /* last backward transition */

	/* Per-instance cfea announce, generated from cfg with OUR src MAC embedded
	 * (on-wire identity must match the L2 source — a mismatch is a documented
	 * slave-disconnect trigger). */
	uint8_t  announce_blk[34];
};

/* Initialize for a given source MAC, console config + frame rate (3675/4000/
 * 8000 fps). `src` is OUR master L2 MAC (Roland OUI); it is stamped into the
 * generated cfea so the announced identity equals the L2 source. `cfg` selects
 * the box I/O the downstream advertises; NULL -> the S-1608 default (8 out /
 * 16 in / M-300 console field), which reproduces the captured M-300 downstream
 * byte-for-byte. Starts in IDLE; the first reac_master_next() enters PROBING. */
void reac_master_init(struct reac_master *m, const uint8_t src[6],
                      const struct reac_console_cfg *cfg, int fps);

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
 * pass to reac_master_stamp (chanmap frame 0..N-1 for CHANMAP; 0 otherwise).
 * Call exactly once per emitted downstream frame.
 * Safety fallbacks (grant-window expiry, peer-gone budget) move the state
 * BACKWARD to PROBING here — no timer ever advances toward ESTABLISHED. */
enum reac_master_emit reac_master_next(struct reac_master *m, uint16_t *counter,
                                       int *tmpl_idx);

/* Stamp the control block for `emit` into a downstream frame already built by
 * reac_tx_build (1492 B: hdr + audio + C2 EA tail). For FILLER this is a no-op.
 * For the cdea/cfea kinds it overwrites type [16:18] + control block [18:50]
 * and applies the checksum, leaving audio + counter + tail intact. GRANT echoes
 * m->join_blk verbatim; CHANMAP/ANNOUNCE use the generated m->chanmap[idx] /
 * m->announce_blk; PROBE/SUB01/SUB02 are the fixed M-300 protocol constants.
 * `tmpl_idx` selects the chanmap frame (0..N-1); ignored for other kinds.
 * Returns 0, or -1 on a bad kind/index. */
int reac_master_stamp(const struct reac_master *m, uint8_t *frame,
                      enum reac_master_emit emit, int tmpl_idx);

/* Human-readable names for the caller's logging. */
const char *reac_master_state_name(enum reac_master_state s);
const char *reac_master_rx_event_name(enum reac_master_rx_event e);
const char *reac_master_drop_name(enum reac_master_drop_reason r);

#endif /* REAC_MASTER_H */
