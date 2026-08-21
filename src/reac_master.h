// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_master — the MASTER-role downstream control plane.
 *
 * reac_ctrl/reac_fsm are the SLAVE half (a virtual stagebox responding to a real
 * master). This module is the inverse: we ARE the REAC master so a real Roland
 * stagebox slaves to us. It drives the master's establishment state machine —
 * probe -> grant -> established + the periodic channel-map/announce — by writing
 * the real cdea/cfea control block into the 32-byte block [18:50] of the
 * downstream broadcast frame that libreac's reac_downstream_build builds.
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
 *
 * The transition DECISIONS live in an explicit (state, event) -> edge table
 * (reac_master_fsm.h, spec docs/MASTER-FSM.md); reac_master_rx and the cadence
 * are its event producers, the enter_* functions its entry actions. One
 * deliberate forward timer exists since the 2026-07-12 rig fix: GRANTING
 * self-completes to ESTABLISHED once the full ENROLL + dwell + sweep is
 * delivered (GRANTING is only ever entered on a validated box frame, so this
 * is not granting into silence; the established peer-gone budget is the
 * backward safety). All other timers still only move BACKWARD to PROBING. */
#ifndef REAC_MASTER_H
#define REAC_MASTER_H

#include <stdint.h>
#include <stddef.h>

#include "reac_slots.h"   /* the two slot spaces: audio fabric vs head-amp */
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
	REAC_M_DROP_BOX_UNKNOWN,   /* the box latched but never declared WHAT it is,
	                            * so we had no width to enroll and refused to
	                            * invent one. Back to probing; it may re-join.  */
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

/* The CHANMAP is a RING of 49 positions: head-amp channels 0x00..0x2f (48) followed
 * by the 0xfe section marker at the wrap. A channel-map frame advertises 8 consecutive
 * ring positions, and a real master emits ONE window per start position — so the
 * full sweep is exactly 49 frames (measured live off an M-200 driving an S-1608,
 * 2026-07-11, #130). An earlier 11-frame figure came from an M-300 capture too
 * short to contain the whole rotation.
 *
 * This ring spans the HEAD-AMP slot space, not the 40-slot AUDIO fabric — it was
 * spelled REAC_M_FABRIC_RING, which said "fabric" while counting head-amp channels
 * (#69). Both spaces are defined once in reac_slots.h. */
#define REAC_M_CHANMAP_RING       REAC_HEADAMP_RING   /* 49 = 48 slots + 0xfe */
#define REAC_M_CHANMAP_FRAMES_MAX REAC_M_CHANMAP_RING

/* Console I/O config: everything the downstream generator needs to synthesize
 * the chanmap + cfea. The master MAC is NOT here — it is OUR L2 source MAC
 * (passed to reac_master_init), which the cfea embeds so the on-wire announced
 * identity always equals the L2 source (a mismatch is a documented
 * slave-disconnect trigger).
 *
 * THIS IS THE MASTER'S OWN IDENTITY, NEVER A BOX DECLARATION (2026-08-05). The
 * box on the wire is learned from the wire and from nowhere else — see the
 * "no box known" contract on reac_master_init. `in_channels` used to seed the
 * grant allocation, which made a compile-time constant the origin of every
 * head-amp slot address; it is gone. What is left here is what the master
 * announces about ITSELF plus the width byte the cfea carries, which
 * reac_master_set_box re-stamps from the RECOGNIZED box. */
struct reac_console_cfg {
	uint8_t out_channels;   /* cfea outCh [18]: the CONNECTED box's input width
	                         * once one is recognized (reac_master_set_box
	                         * re-stamps it); the idle placeholder until then.
	                         * (Does NOT size the chanmap: a real master sweeps
	                         * the whole 48-slot chanmap ring regardless of
	                         * console width, #130 — that ring is the HEAD-AMP
	                         * space, not the 40-slot audio fabric, see
	                         * reac_slots.h.)                                 */
	uint8_t console_field;  /* cfea [19] and [21] (move together): the emulated
	                         * MASTER model. M-300 = 0x00, M-5000 = 0x01.     */
};

/* The IDLE console: what the master announces about itself before any box has
 * declared itself on the wire. out_channels is the cfea width byte's placeholder
 * (0x08, the value every captured desk announces while unlinked); console_field 0
 * is the V-Mixer identity. reac_master_init(cfg == NULL) uses this.
 *
 * It declares NO BOX. There is deliberately no in_channels here any more: a box
 * width in a compile-time constant is a box we have never seen, and it used to
 * be the origin of the grant allocation and hence of every head-amp slot address
 * (see reac_master_init). */
#define REAC_CONSOLE_CFG_IDLE \
	((struct reac_console_cfg){ .out_channels = 8, .console_field = 0 })

/* A MIXER PROFILE — the desk generation reac-pw speaks as. The grant burst is
 * box-defined (a box locks to any valid grant), so the only per-mixer identity
 * is the console-model byte (0 = V-Mixer M-200/M-300, 1 = OHRCA M-5000), which
 * drives BOTH the cfea [19] and the ENROLL console byte (the same 0/1
 * indicator, measured across matrix-m{200,300,5000}-*). The source MAC is NOT
 * a profile field: the master emits from THIS NIC's own address (reac_mac.h;
 * the conformance suite pins identity == the L2 source). Probe specials +
 * cadence are currently the V-Mixer (M-200) set for every profile — a box
 * still locks, but that is the remaining per-mixer fidelity item. */
struct reac_mixer_profile {
	const char *name;        /* CLI token: "m200" | "m300" | "m5000"          */
	const char *display;     /* "M-200" ...                                   */
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
 * THE FAMILY IS DETACHED FROM THE PACE (operator, 2026-08-21). A Roland desk —
 * V-Mixer or OHRCA alike — offers 44.1, 48 and 96 kHz in its REAC menu and
 * drives the segment at whichever the operator chose; the identity byte says
 * which desk we impersonate, nothing about the rate. So the pace is a
 * CONFIGURED SETTING THAT MUST BE OBEYED, for every profile, and only those
 * three values are legal — anything else needs re-pacing between the rig clock
 * and the wire, which reac-pw cannot do.
 *
 * This comment used to claim the opposite: that a V-Mixer identity pinned the
 * segment to 48 kHz and OHRCA was natively 96. reac_mixer_resolve_rate stopped
 * believing that some time ago (it ignores `mixer` entirely) and the comment was
 * never corrected, so the header and the code have been contradicting each other
 * — the header describing a rule the function does not implement.
 *
 * A DIVERGENCE IS OPEN AGAINST THIS LAW, measured 2026-08-21 and reproducible:
 * with `--rate 96000` our TX paced 8001 fps under both profiles, and the S-0808
 * returned 8006 fps (96 k) as an OHRCA master but 4002 fps (48 k) as a V-Mixer
 * one — the box halved its return, and the resulting two-pace mismatch is
 * audible as granulated, saturated audio. Our side obeys the setting; the box
 * does not follow it. Unexplained, and NOT a licence to re-derive the old
 * identity-selects-rate rule: it is filed as a divergence, not a design.
 *
 * `requested` is the --rate value (0 = unset/auto). Returns the rate reac-pw
 * should actually emit at. If `clamped` is non-NULL, sets *clamped to 1 when
 * `requested` was non-zero and got overridden (the caller should warn), else 0. */
int reac_mixer_resolve_rate(const struct reac_mixer_profile *mixer, int requested, int *clamped);

/* The cfea[19] "console" byte is the segment's RATE CLASS, not the desk's name.
 *
 * MEASURED 2026-08-21, and it is the whole story: emitting m5000 vs m200 at the
 * same --rate 96000 changes exactly ONE byte on the wire — cfea block[19], 0x01
 * vs 0x00, plus its checksum. Probe, sub01, sub02, chanmap, ENROLL and the grant
 * burst are byte-identical. And the box's pace follows that byte: 0x01 -> it
 * returns 8004 fps (96 kHz), 0x00 -> 4002 fps (48 kHz), with our own TX pacing
 * 8001 fps in both cases.
 *
 * WHAT IS ESTABLISHED, AND WHAT IS NOT. The rig behaviour above is reproducible
 * and is why this byte is derived from the RESOLVED RATE — 96 kHz -> 1,
 * 44.1/48 kHz -> 0 — so that under the law THE FAMILY IS DETACHED FROM THE PACE,
 * --rate 96000 reaches the segment whichever generation --mixer names.
 *
 * But the byte's MEANING is not settled, and this comment does not pretend it is.
 * Across the capture corpus it is CONSTANT PER DESK MAC (M-200i 0x00, M-300 0x00,
 * M-5000 0x01), and every desk in the corpus only ever ran ONE rate — the V-Mixers
 * at 48 kHz, the M-5000 at 96 — so "rate class" and "family" predict the corpus
 * identically and it cannot separate them.
 *
 * A CORRECTION WORTH KEEPING: the corpus first appeared to show M-200i frames at
 * 8000 fps carrying 0x00, which would have refuted the rate-class reading outright.
 * It is the MIRROR-TAP ARTIFACT (see the correction in reac-captures and libreac
 * reac.h:35): those captures were taken through a port mirroring both directions,
 * and counting frames without collapsing same-counter pairs doubles the apparent
 * rate. Measured properly, 50% of the desk frames are duplicates and the real rate
 * is 4000 fps = 48 kHz. ALWAYS dedupe by the counter at frame[14:16] before
 * calling a capture's rate.
 *
 * TO SETTLE IT: an unambiguous capture of a REAL desk at a rate its family does
 * not usually run — an M-300 (c9:d8:5b, never impersonated) at 96 kHz, or an
 * M-5000 (ca:15:4c) at 48 kHz. The corpus has neither. Until then this is a
 * rig-determined behaviour that satisfies the law, not a decoded field. Two
 * alternatives ARE excluded: the box's pace does not follow our source MAC (a
 * Roland-OUI --src-mac with 0x00 still returned 48 kHz), and it does not follow
 * our TX cadence alone (we paced 8007 fps and it answered 4004).
 *
 * What the box does with 44.1 vs 48 (both class 0) is not established here —
 * no capture separates them, and this returns 0 for both rather than guess. */
uint8_t reac_rate_console_field(int rate);

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
	int      enroll_pending;  /* set by reac_master_set_box when the box's DECLARED
	                           * width narrowed enroll_blk after the initial ENROLL;
	                           * the GRANTING dwell re-emits ONE ENROLL at the new
	                           * width (the box widens/narrows to it, matching the
	                           * golden's post-recognition enrol) then clears this. */
	int      grant_dwell;     /* dwell slots between ENROLL and the grant burst
	                           * (fps*REAC_M_GRANT_DWELL_SECONDS_X10/10, set at init) */
	struct reac_grant_alloc alloc;   /* THE box identity: the head-amp slots we
	                           * allocated to the box that is ON THIS WIRE. width
	                           * == 0 means NO BOX IS KNOWN — a normal state, not
	                           * an error (reac_master_has_box). Written ONLY by
	                           * reac_master_set_box (recognition) and cleared by
	                           * reac_master_forget_box (every fall back to
	                           * PROBING). Nothing else may seed it.            */
	uint8_t  grant_burst[REAC_GRANT_SWEEP_MAX][34];  /* the generated sweep     */
	int      grant_burst_len; /* rows in grant_burst                            */
	const struct reac_headamp_tx *headamp_src;  /* head-amp state group A pushes;
	                           * NULL -> every channel takes the safe default    */
	uint8_t  join_blk[32];    /* the box's cold-connect block (diagnostic)      */
	uint8_t  box_mac[6];      /* the joining box's L2 source */
	/* Bumped every time we reach ESTABLISHED. A box plug is not a special case:
	 * it is a lost connection and a reconnect, and it comes in two flavours the
	 * MAC alone cannot tell apart. A DIFFERENT MAC is a cold reconnect — new
	 * peer, new declared geometry, nodes resize. The SAME MAC returning is a warm
	 * one — the geometry stands, but it is still a NEW SESSION whose counter
	 * starts wherever the box's did. Keying stream state on the MAC alone leaves
	 * the warm case carrying the old session's continuity, which is how a clean
	 * reconnect reported thousands of counter gaps. */
	unsigned session_seq;
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
 * generated cfea so the announced identity equals the L2 source. `cfg` is the
 * master's OWN identity (console model + the idle cfea width byte); NULL ->
 * REAC_CONSOLE_CFG_IDLE. Starts in IDLE; the first reac_master_next() enters
 * PROBING.
 *
 * IT STARTS WITH NO BOX (2026-08-05, the operator's ruling: "we should not start
 * reac-pw with a preconfigured box, it has to be dynamic"). alloc.width == 0 and
 * grant_burst_len == 0 until a real box declares itself on the wire and
 * reac_master_set_box is called with what it declared. That is a NORMAL running
 * state — reac-pw comes up, probes, and waits for a box, exactly as an unlinked
 * desk does.
 *
 * WHAT THIS REPLACES, and why it is not merely tidier. init used to allocate the
 * grant sweep from cfg.in_channels, whose only ever value was the S-1608's 16 —
 * hard-coded in reac_sink_node.c, reachable from no CLI flag. So the head-amp
 * slot addresses of a box we had never seen came from a compile-time constant:
 * base 0x20, width 16. A box that reached GRANTING before declaring its model
 * (the cold-connect JOIN carries no width) was granted THAT enrollment. If it was
 * not a 16-input box, the grant claimed slots it does not own and claimed none
 * that it does — link established, audio fine, every later head-amp record
 * ignored (the 2026-07-17 live failure recorded in reac_grant.h). Recognition
 * DID correct it a moment later on every box in the fixed matrix, which is why
 * this stayed latent; a box outside the matrix, or one whose config-announce is
 * lost, had nothing to correct it. Now there is no fabricated box to fall back
 * to, so the wire is the only source and the failure cannot be silent. */
void reac_master_init(struct reac_master *m, const uint8_t src[6],
                      const struct reac_console_cfg *cfg, int fps);

/* Is a real box's declaration currently in force? 0 = none known (the startup
 * state, and the state after every drop). The grant sweep is empty while this is
 * 0, and the master will not enroll anything — see reac_master_next's GRANTING
 * hold. */
int reac_master_has_box(const struct reac_master *m);

/* Allocate head-amp slots for the box THE WIRE DECLARED (the recognizer, #137)
 * and REGENERATE the grant sweep for that allocation. Keyed on the box's INPUT
 * width (the sweep enrolls the box's inputs; S-0808 and S-1608 are both 8-OUT, so
 * the output count does not distinguish them). This is the ONLY way a box width
 * ever enters the master. A width we cannot place leaves the master's box
 * identity untouched — including the enroll/cfea width bytes, which used to be
 * stamped from the bad width even as the allocation was refused. Call from the
 * FSM-owning thread on a box recognition. */
void reac_master_set_box(struct reac_master *m, int in_ch, int out_ch);

/* Forget the box: no allocation, no sweep, the wide-safe ENROLL and the idle
 * cfea back. Called on EVERY backward transition to PROBING (enter_probing), so
 * "PROBING" means "we know nothing about any box" by construction.
 *
 * This is what makes a box SWAP safe. Recognition only fires on a CHANGE of
 * matched model, so without forgetting, a box that leaves and is replaced by one
 * whose config-announce we cannot match would inherit the departed box's base
 * and be enrolled at slots it does not own — the same silent failure, one box
 * removed. Re-derivation is then unconditional: whatever joins next declares
 * itself from scratch. */
void reac_master_forget_box(struct reac_master *m);

/* Re-fire the grant burst for the currently-recognized box (m->box_mac), so a width
 * learned AFTER the cold-connect JOIN (reac_master_set_box, from the box's config-
 * announce) is actually DELIVERED on the wire. Without it the box keeps its JOIN-time
 * default enrollment; a wider box (S-4000S) stays at its 8-ch cold-connect floor. */
void reac_master_regrant(struct reac_master *m);

/* Point the grant sweep's group A at the head-amp state to push on enrollment.
 * `tx` is BORROWED (not copied) and must outlive `m`; NULL -> the safe defaults.
 * Regenerates the sweep immediately so a later grant enrolls the current state.
 * The pacer owns both the master and the head-amp table on one thread, so no
 * locking is implied. Call from the FSM-owning thread. */
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
 * reac_downstream_build (1492 B: hdr + audio + C2 EA tail). For FILLER this is a no-op.
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
