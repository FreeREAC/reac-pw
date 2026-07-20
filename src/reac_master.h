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

#include "reac_grant.h"   /* struct reac_grant_alloc, REAC_GRANT_SWEEP_MAX */

struct reac_headamp_tx;   /* reac_headamp_tx.h — the head-amp state group A pushes */

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
 *                 grant burst (1 frame per 12 slots over a ~150 ms window),
 *                 preceded by a ~1.6 s ENROLL->grant DWELL (grant_dwell) that
 *                 matches the measured M-200 gap (see grant_dwell's comment in
 *                 reac_master_init). -> ESTABLISHED on the box's first
 *                 unicast-to-us frame of any kind; window expiry with no
 *                 unicast falls BACK to PROBING.
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
	REAC_M_EMIT_GRANT,      /* cdea 04 03 — one block of the model grant burst  */
	REAC_M_EMIT_ENROLL,     /* cdea 01 03 000d — the pre-grant enroll/arm frame */
	REAC_M_EMIT_CHANMAP,    /* cdea 01 03 0019 generated channel-map (1 of N)   */
	REAC_M_EMIT_ANNOUNCE,   /* cfea master announce (generated: OUR MAC + I/O)  */
};

/* RX events the pacer feeds in (classified by reac_ctrl_classify_box_frame). */
enum reac_master_rx_event {
	REAC_M_RX_BOX_BCAST_FILLER = 0, /* box presence-flood (diagnostic only)     */
	REAC_M_RX_BOX_JOIN,             /* validated box cdea 04 03 cold-connect    */
	REAC_M_RX_BOX_UNICAST,          /* any unicast-to-us box frame (audio/…)    */
	REAC_M_RX_BOX_HEARTBEAT,        /* box cdea 01 03 0001 sel 0x81 keep-alive —
	                                * the box's ESTABLISHED signal ("I am locked").
	                                * Symmetric to the heartbeat our SLAVE emits in
	                                * FSM_ESTABLISHED; its ARRIVAL is the definitive
	                                * confirmation the real box has locked to us.   */
	REAC_M_RX_BOX_BYE,              /* box heartbeat with selector 0x00         */
	REAC_M_RX_BOX_CONFIG,          /* box config-announce cdea 01 03 0010 — the
	                                * box declaring its setup; establishes even on
	                                * a WARM RELINK (no cold-connect JOIN)        */
};

/* Why the last backward transition happened (for the caller's logging). */
enum reac_master_drop_reason {
	REAC_M_DROP_NONE = 0,
	REAC_M_DROP_PEER_GONE,     /* established 600-frame budget drained          */
	REAC_M_DROP_BYE,           /* explicit box disconnect (hb selector 0x00)    */
	REAC_M_DROP_MAC_CHANGE,    /* JOIN from a different box — re-grant the new  */
	REAC_M_DROP_GRANT_TIMEOUT, /* grant window expired with no box unicast      */
};

/* Established link-check budget. The firmware constant 0x0258 = 600 frames
 * (~0.15 s @48k) was FAR too eager: a live M-200i driving an S-1608 held the link
 * for ~6.5 s of box silence before reverting to hunting (measured on a reboot,
 * 2026-07-11 — box heartbeat stops at t=16.0 s, master's first probe at t=22.47 s).
 * We reload a rate-scaled ~6.5 s (reac_master.link_check_reload, set at init) so a
 * briefly-glitching box isn't torn down the way a real desk would ride through.
 * The old constant is kept only as documentation of the firmware value. */
#define REAC_M_LINKCHECK_RELOAD 600
#define REAC_M_LINKCHECK_SECONDS_X10 65   /* 6.5 s, scaled by fps at init */
/* Diagnostic presence flag decay (same frame budget as the link-check). */
#define REAC_M_PRESENCE_TIMEOUT 600
/* Grant burst density: one echoed grant per this many slots (~100 control
 * frames over the ~150 ms window @8000 fps, the transcribed real burst). */
#define REAC_M_GRANT_STRIDE 12
/* ENROLL->grant DWELL: a real M-200 holds ~1.6 s between the ENROLL arm frame
 * and the start of the grant burst (measured Δ1.503 s on
 * matrix-m200-s0808-2026-07-11.pcap, Δ1.717 s on matrix-m200-s1608-2026-07-11.pcap,
 * both real M-200 goldens). Frame-counted (not wall-clock — the pacer is
 * tick-driven) and fps-scaled at init like link_check_reload. */
#define REAC_M_GRANT_DWELL_SECONDS_X10 16   /* 1.6 s, scaled by fps at init */

/* Post-establish scene COMMIT (REACPW_EST_COMMIT, default OFF). At scene recall a
 * real M-200, WHILE ESTABLISHED, re-pushes the full head-amp scene and then emits an
 * ordered SUB01 -> SUB02 pair; that pair drives the box's scene-FSM (FUN_0c0037ee)
 * to its state-4 bulk commit (FUN_0c003c8a), which copies the head-amp staging into
 * active for EVERY slot and flushes the phantom groups to hardware. reac-pw only ever
 * emitted SUB01/SUB02 while PROBING, so it never fired the box's commit and only the
 * anchor input latched 48V. When the flag is on, enter_established arms est_commit to
 * (fps/SCENE_SETTLE_DEN + SUB_GAP) FILLER-eligible slots: SUB01 fires ~fps/SCENE_SETTLE_DEN
 * slots in (well after the post-establish scene re-push has settled), SUB02 SUB_GAP
 * slots later. OFF leaves est_commit 0, so the locked cadence is byte-identical to today.
 *
 * SUSTAIN, not one-shot (2026-07-21): a real WORKING console does not fire this pair
 * once — it SUSTAINS SUB01->SUB02 continuously on the established cadence (a committing
 * S-0808 capture shows ~134 SUB events over 79.5 s, ~1.7/s). A one-shot pair only ever
 * flushes the box's STAGING->ACTIVE table the instant it fires, so any non-anchor input
 * whose staging changed after that (or was never in place at the first flush) never
 * commits. control_cadence's ESTABLISHED branch now RE-ARMS est_commit to fps/PERIOD_DEN
 * FILLER-eligible slots the moment SUB02 fires, so the ordered pair repeats for the whole
 * ESTABLISHED lifetime instead of disarming. PERIOD_DEN=1 => ~fps slots between pairs,
 * i.e. ~1 pair/sec — inside the ~1.7/s real flood, so it never outruns the box's own
 * commit rate. Still flag-gated: OFF never arms est_commit in the first place, so the
 * re-arm branch is unreachable and inert. */
#define REAC_M_EST_COMMIT_SCENE_SETTLE_DEN 4   /* SUB01 after ~fps/4 (~250 ms) FILLER slots */
#define REAC_M_EST_COMMIT_SUB_GAP          8   /* FILLER slots between SUB01 and SUB02 */
#define REAC_M_EST_COMMIT_PERIOD_DEN       1   /* sustain re-arm: fps/DEN FILLER-eligible
                                                 * slots between SUB pairs (~1/s);
                                                 * PERIOD_DEN=1 keeps it under the observed
                                                 * ~1.7/s real cadence */
#define REAC_M_EST_COMMIT_LIVE_SETTLE      4   /* small settle before a LIVE head-amp
                                                 * re-arm's SUB01 (reac_master_set_headamp_src) */

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

/* A MIXER PROFILE — the desk reac-pw impersonates. The grant burst is box-defined
 * (a box locks to any valid grant), so the only per-mixer identity is a small set
 * of fields: the master MAC and the console-model byte (0 = V-Mixer M-200/M-300,
 * 1 = OHRCA M-5000), which drives BOTH the cfea [19] and the ENROLL console byte
 * (they carry the same 0/1 indicator, measured across matrix-m{200,300,5000}-*).
 * Probe specials + cadence are currently the V-Mixer (M-200) set for every profile
 * — a box still locks, but that is the remaining per-mixer fidelity item. */
struct reac_mixer_profile {
	const char *name;        /* CLI token: "m200" | "m300" | "m5000"          */
	const char *display;     /* "M-200" ...                                   */
	uint8_t     mac[6];      /* the desk's captured master MAC (default id)    */
	uint8_t     console_field; /* cfea [19] + ENROLL console byte: 0=V-Mixer,1=OHRCA */
};

/* Look up a profile by CLI token; NULL if unknown. */
const struct reac_mixer_profile *reac_mixer_profile_by_name(const char *name);
/* Enumerate profiles for --help (index 0..N-1; NULL past the end). */
const struct reac_mixer_profile *reac_mixer_profile_at(int i);

/* Resolve the master's EMISSION rate for a requested --rate against the
 * impersonated mixer profile (task #156, "96k is not anything different, same
 * state diagram, doubled frequency" — parameterizing the existing 48k path by
 * mixer profile rather than re-engineering the FSM).
 *
 * A V-Mixer desk (console_field 0: M-200/M-300) only ever exists on the wire
 * at 48 kHz: cfea/ENROLL carry no explicit rate field, so a box infers 48 kHz
 * purely from the V-Mixer identity (docs/MASTER-HARDWARE-VERIFY.md, "Sample
 * rate is the desk MODEL, not a clock knob") — reac-pw forces 48 kHz and
 * ignores/reports a mismatched --rate, exactly as before this task. An OHRCA
 * desk (console_field 1: M-5000) is native 96 kHz; its downstream frame shape
 * is IDENTICAL to the V-Mixer's (REAC_FRAME_BYTES, unchanged — see the #156
 * trailer RE in reac_tx.h/tests/test_reac_tx.c: the "1494 B OHRCA frame" some
 * captures show is a mirror-capture artifact, not a real field), so unlike the
 * V-Mixer there is nothing tying it to a fixed rate — `requested` is honored,
 * defaulting to the native 96 kHz when unset.
 *
 * `requested` is the --rate value (0 = unset/auto). Returns the rate reac-pw
 * should actually emit at. If `clamped` is non-NULL, sets *clamped to 1 when
 * `requested` was non-zero and got overridden (the caller should warn), else 0. */
int reac_mixer_resolve_rate(const struct reac_mixer_profile *mixer, int requested, int *clamped);

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

	/* GRANTING — hold for grant_dwell slots (ENROLL->grant DWELL, ~1.6 s,
	 * fps-scaled at init) after the ENROLL arm frame, THEN emit the master's own
	 * grant sweep (the cdea 04 03 per-channel enrollment burst), one block per
	 * grant_stride slots, then -> ESTABLISHED.
	 *
	 * The sweep is GENERATED (reac_grant_build_sweep) from `alloc` — the fabric
	 * slots WE allocated to the connected box — not replayed from a captured
	 * table. See reac_grant.h: group A is the head-amp state push for the
	 * allocated channels, so a replayed sweep enrolls the wrong slots and every
	 * later head-amp record is meaningless to the box (live 2026-07-17). It is
	 * rebuilt on box recognition (reac_master_set_box) and whenever the head-amp
	 * source changes (reac_master_set_headamp_src). */
	int      grant_ticks;     /* slots elapsed in the current grant window */
	int      grant_dwell;     /* dwell slots between ENROLL and the grant burst
	                           * (fps*REAC_M_GRANT_DWELL_SECONDS_X10/10, set at init) */
	struct reac_grant_alloc alloc;   /* the fabric slots we granted this box    */
	uint8_t  grant_burst[REAC_GRANT_SWEEP_MAX][34];  /* the generated sweep     */
	int      grant_burst_len; /* rows in grant_burst                            */
	const struct reac_headamp_tx *headamp_src;  /* head-amp state group A pushes;
	                           * NULL -> every channel takes the safe default    */
	uint8_t  join_blk[32];    /* the box's cold-connect block (diagnostic)      */
	uint8_t  box_mac[6];      /* the joining box's L2 source */
	unsigned grant_attempts;  /* windows opened (diagnostic) */

	/* ESTABLISHED */
	int      chanmap_cursor;  /* which generated chanmap frame is next (0..N-1) */
	int      est_chanmap_tick; /* LOCKED-state chanmap heartbeat counter. A real
	                            * M-200 runs the chanmap at a metronomic 1.00/s
	                            * once locked (measured matrix-m200-s0808: 1004 ms
	                            * gaps) — the box's sync keep-alive. The slower
	                            * 1/cycle hunt rate (~0.37/s) left the box BLINKING
	                            * (rig 2026-07-12). Phase-offset from cfea.        */
	int      link_check;        /* countdown to peer-gone */
	int      link_check_reload; /* ~6.5 s of frames (fps-scaled), the reload value */

	/* One-shot post-establish scene COMMIT countdown (REACPW_EST_COMMIT, default
	 * OFF). Armed by enter_established ONLY when the flag is on (else 0 = inert);
	 * counts down the FILLER-eligible ESTABLISHED slots and drives the ordered
	 * SUB01 -> SUB02 pair that fires the box's scene-FSM state-4 bulk phantom commit
	 * (see control_cadence). 0 when disarmed -> the LOCKED cadence is byte-identical
	 * to today. */
	int      est_commit;

	/* Diagnostics (never gate the establishment) */
	int      box_seen;        /* sustained box broadcast FILLER on the wire */
	int      presence_tick;   /* countdown to clearing box_seen */
	enum reac_master_drop_reason drop_reason;  /* last backward transition */

	/* Per-instance cfea announce, generated from cfg with OUR src MAC embedded
	 * (on-wire identity must match the L2 source — a mismatch is a documented
	 * slave-disconnect trigger). */
	uint8_t  announce_blk[34];

	/* Per-instance ENROLL (cdea 01 03 000d), built from the mixer profile: the
	 * console-model byte [8] = cfg.console_field (0 = V-Mixer, 1 = OHRCA). */
	uint8_t  enroll_blk[34];
};

/* Initialize for a given source MAC, console config + frame rate (3675/4000/
 * 8000 fps). `src` is OUR master L2 MAC (Roland OUI); it is stamped into the
 * generated cfea so the announced identity equals the L2 source. `cfg` selects
 * the box I/O the downstream advertises; NULL -> the S-1608 default (8 out /
 * 16 in / M-300 console field), which reproduces the captured M-300 downstream
 * byte-for-byte. Starts in IDLE; the first reac_master_next() enters PROBING. */
void reac_master_init(struct reac_master *m, const uint8_t src[6],
                      const struct reac_console_cfg *cfg, int fps);

/* Allocate fabric slots for the AUTODETECTED box (the recognizer, #137) and
 * REGENERATE the grant sweep for that allocation. Keyed on the box's INPUT width
 * (the sweep enrolls the box's inputs; S-0808 and S-1608 are both 8-OUT, so the
 * output count does not distinguish them). A width we cannot place keeps the
 * current allocation + sweep and the caller should log the fallback. Call from
 * the FSM-owning thread on a box recognition. */
void reac_master_set_box(struct reac_master *m, int in_ch, int out_ch);

/* Point the grant sweep's group A at the head-amp state to push on enrollment.
 * `tx` is BORROWED (not copied) and must outlive `m`; NULL -> the safe defaults.
 * Regenerates the sweep immediately so a later grant enrolls the current state.
 * The pacer owns both the master and the head-amp table on one thread, so no
 * locking is implied. Call from the FSM-owning thread.
 *
 * REACPW_EST_COMMIT (default OFF): when on and the FSM is already ESTABLISHED,
 * also re-arms est_commit for a PROMPT fresh SUB01->SUB02 pair (SUB_GAP + a small
 * settle, not the full post-establish SCENE_SETTLE_DEN wait) so a LIVE head-amp
 * edit re-flushes the box's STAGING->ACTIVE table right away instead of waiting
 * for the sustained cadence's next scheduled re-arm. No-op with the flag off or
 * before ESTABLISHED (est_commit is left untouched — 0 when the flag is off). */
void reac_master_set_headamp_src(struct reac_master *m,
                                 const struct reac_headamp_tx *tx);

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

/* REACPW_EST_COMMIT (default OFF) flag accessor — read once + cached. Exposed so the
 * pacer's open-time head-amp seed and enter_established's est_commit arming read the
 * SAME flag (single source of truth). Returns 1 when the post-establish scene commit
 * is enabled, else 0. */
int reac_master_est_commit_enabled(void);

/* Human-readable names for the caller's logging. */
const char *reac_master_state_name(enum reac_master_state s);
const char *reac_master_rx_event_name(enum reac_master_rx_event e);
const char *reac_master_drop_name(enum reac_master_drop_reason r);

#endif /* REAC_MASTER_H */
