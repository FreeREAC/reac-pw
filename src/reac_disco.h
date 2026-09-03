// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_disco — passive REAC device discovery: WHAT IS ON THIS SEGMENT, as opposed
 * to what this master has joined.
 *
 * Why this is a separate module from reac_ctrl's classifier. reac_ctrl_classify_box_frame
 * answers "what does the master FSM do with this frame?", so it deliberately DISCARDS
 * everything the FSM cannot act on — another master's broadcast (reac_ctrl.c:141) and
 * unicast between third parties (reac_ctrl.c:144). Those discards are exactly the
 * sightings discovery exists to report. Rather than widen a classifier the live master
 * depends on, discovery re-reads the frame independently: reac_disco_classify is pure,
 * ownership-blind, and its verdict never reaches the FSM.
 *
 * Presence is NEVER a heuristic. A sighting requires a 0x8819 frame and (for non-FILLER) a
 * verified checksum. A packet COUNT is not evidence — openmixer #161 announced any NIC over
 * ~62 pkt/s (ordinary Wi-Fi) as a REAC master because an rx_packets delta was fed in as a
 * device.
 *
 * NO MAC-BASED TRUST, EVER (operator's ruling, 2026-09-03): a sighting is never gated on the
 * source MAC's vendor prefix. The Roland-OUI check that used to sit here was deleted outright
 * — REAC gear is discovered by PROTOCOL FRAME alone, never by pinning or spoofing a MAC.
 * `our_mac` above still excludes our OWN echo (reac_disco_classify's self-filter, unrelated to
 * vendor trust — it stops a hub/loopback re-delivering our own traffic, whatever the trust
 * policy).
 *
 * THE FILLER GAP THIS LEFT, AND HOW IT IS CLOSED. FILLER (type 0000, audio) frames are
 * checksum-EXEMPT, so with the OUI gone a bare FILLER sighting rests on the EtherType/kind
 * parse and the self-echo filter alone — not enough to refuse a spoofed or noise frame
 * claiming to be a box. The operator's ruling: REAC is physically POINT-TO-POINT — two boxes
 * cannot collide on the same segment/NIC — so use SEGMENT-LEVEL PEER-LOCKING instead of any
 * MAC-vendor scheme. reac_disco_classify_on_segment remembers, per segment, the source MAC of
 * the FIRST sighting a real control frame validated (0x8819 parse + a VERIFIED checksum — the
 * existing non-FILLER path, unchanged); once a segment has that proven peer, a FILLER frame on
 * the SAME segment is accepted only if its source matches it. This is NOT identity/vendor
 * checking — nothing is hardcoded or configured — it is "is this the box already proven real
 * on THIS wire", learned dynamically. reac_disco_classify (no lock) keeps its old, unlocked
 * behaviour for a caller with no segment concept (existing callers, unit tests).
 * (Real-hardware check, 2026-09-03: neither REAC-BOX-STATE-DIAGRAM.md nor the golden captures
 * document a structural check — a sequence-counter continuity requirement or "FILLER length
 * must equal the box's declared join width" — that a real console applies to its OWN
 * established box's FILLER stream; the counter field reac_frame_counter() exposes is used
 * elsewhere in this project only for LOSS COUNTING on an already-trusted link, never as an
 * acceptance gate. The likely reason: a real box is wired point-to-point, so a real console
 * never needs to defend against a foreign FILLER at all — the physical topology already
 * guarantees it. reac-pw's segment is only PHYSICALLY point-to-point when nothing else shares
 * the VLAN/trunk it listens on, which the peer-lock above defends in software instead.)
 *
 * Threading: reac_disco_classify / reac_disco_classify_on_segment are pure and RT-safe (no
 * alloc, no stdio) — the pacer thread calls them, one `reac_disco_peer_lock` per segment's own
 * pacer (so its lifetime matches the segment's: a drop/reopen gets a fresh, unlocked lock).
 * The TABLE is main-thread-only: sightings cross threads on the pacer's existing lock-free
 * event ring, so there is no new cross-thread primitive here. See
 * docs/design/specs/2026-07-16-reac-discovery-via-reac-pw.md (openmixer) for the seam. */
#ifndef REAC_DISCO_H
#define REAC_DISCO_H

#include <stdint.h>
#include <stddef.h>

#include "reac_ctrl.h"

/* Most REAC segments carry one master + a handful of boxes. The table is fixed-size
 * (RT discipline: no allocation) and saturates rather than evicting — a full table is
 * reported as such, never as a complete picture. */
#define REAC_DISCO_MAX 8

/* How long a MAC may go unheard before it is withdrawn. A box emits FILLER at wire rate
 * and a heartbeat every second, so 5 s of silence is a real disappearance, not a gap.
 * Withdrawal is what lets a vanished stagebox raise an alarm instead of standing stale. */
#define REAC_DISCO_STALE_NS  (5ULL * 1000000000ULL)

/* Re-announce cadence for a MAC already in the table (see reac_disco_should_push). */
#define REAC_DISCO_REFRESH_NS (1ULL * 1000000000ULL)

enum reac_disco_role {
	/* Deliberate: an ambiguous frame is reported as REAC gear of UNKNOWN role rather
	 * than guessed into box or master. Both mistakes mislead the operator. */
	REAC_DISCO_ROLE_UNKNOWN = 0,
	REAC_DISCO_ROLE_BOX,
	REAC_DISCO_ROLE_MASTER,
};

const char *reac_disco_role_name(enum reac_disco_role r);

struct reac_disco_sighting {
	uint8_t mac[6];
	enum reac_disco_role role;
	/* The byte-exact config-block match, or NULL when unidentified. NEVER filled from
	 * reac_box_model_by_channels: that silently defaults an unknown width to S-1608
	 * (reac_ctrl.c:394), which is a sane audio-path fallback and a LIE in a device list. */
	const struct reac_box_model *model;
	/* The peer's DATA-FRAME WIDTH in channels, from the frame length alone; 0 when the frame
	 * carried no legal `52 + n*36` geometry. The role field says what the peer CLAIMS; this
	 * says what it IS, and a stagebox strapped to master mode claims master while emitting a
	 * box width. Absence is 0 and means unknown, never "zero channels". */
	unsigned channels;
};

/* Classify one raw frame into a sighting, blind to whether we own the peer.
 * Returns 0 and fills *out on a sighting; -1 when the frame is not evidence of REAC
 * gear (not 0x8819, our own echo, or a corrupt control block).
 * Pure: no state, no clock, RT-safe. No MAC-based trust of any kind — see reac_disco.h's
 * header comment for the operator's ruling and reac_disco_classify_on_segment below for the
 * FILLER-frame gap that ruling leaves and how it is closed. */
int reac_disco_classify(const uint8_t *frame, size_t len, const uint8_t our_mac[6],
                        struct reac_disco_sighting *out);

/* Per-segment established-peer state for reac_disco_classify_on_segment. One instance per
 * segment's own pacer — its lifetime matches the segment's (a drop/reopen makes a fresh,
 * unlocked one, so a genuine box replacement on that physical port is never stuck refusing
 * the new box's FILLER frames forever). Zero-initialize with reac_disco_peer_lock_init. */
struct reac_disco_peer_lock {
	uint8_t mac[6];
	int locked;
};

void reac_disco_peer_lock_init(struct reac_disco_peer_lock *lock);

/* Same contract as reac_disco_classify, plus segment-level peer-locking: the FIRST sighting a
 * real (checksum-verified) non-FILLER control frame validates latches *lock to that source
 * MAC, once, for *lock's lifetime — a later checksum-verified frame from a DIFFERENT MAC is
 * still accepted unchanged (the lock never restricts a VERIFIED frame, only a checksum-EXEMPT
 * FILLER one). Once locked, a FILLER frame from any OTHER source MAC on the segment is refused
 * (-1): not an identity/vendor test, but "is this the box already proven real on a link that
 * is physically point-to-point". Before *lock is ever locked, a FILLER frame is accepted
 * exactly as reac_disco_classify already accepted it — this only closes the window AFTER a
 * real peer is known. RT-safe, no allocation. */
int reac_disco_classify_on_segment(struct reac_disco_peer_lock *lock, const uint8_t *frame,
                                   size_t len, const uint8_t our_mac[6],
                                   struct reac_disco_sighting *out);

/* Index of a model in the fixed matrix (reac_box_model_table), the form a model takes
 * when it crosses the pacer's event ring: the ring slot carries bytes, not pointers.
 * -1 / 0 == unidentified. The table rows are static and const, so an index is a stable,
 * marshalling-free name for one. */
int reac_disco_model_index(const struct reac_box_model *m);
const struct reac_box_model *reac_disco_model_by_index(int idx);

/* --- the RT-side announce gate -------------------------------------------------
 * Which sightings earn a slot on the pacer's 128-entry event ring.
 *
 * A frame-per-sighting would be catastrophic, not merely wasteful: an established box
 * emits unicast FILLER at wire rate (8000 fps @ 96k), so pushing each one would flood
 * the ring and drop-newest would silently evict the JOIN/BYE blocks the rig transcript
 * exists to capture — the hazard reac_pacer.c already documents for heartbeat logging.
 *
 * So: push on an EDGE (a new MAC, or a sharper role/model), plus one refresh per MAC
 * per REAC_DISCO_REFRESH_NS to drive liveness. <=8 MACs => <=8 events/s.
 * Pacer-thread-local: single-writer, no atomics, no allocation. */
struct reac_disco_gate_entry {
	uint8_t mac[6];
	enum reac_disco_role role;
	int model_idx;
	uint64_t last_push_ns;
};

struct reac_disco_gate {
	struct reac_disco_gate_entry e[REAC_DISCO_MAX];
	int n;
};

void reac_disco_gate_init(struct reac_disco_gate *g);

/* Returns 1 when this sighting must reach the main thread, 0 when it is a duplicate
 * inside the refresh window. RT-safe. */
int reac_disco_gate_should_push(struct reac_disco_gate *g,
                                const struct reac_disco_sighting *s, uint64_t now_ns);

struct reac_disco_entry {
	uint8_t mac[6];
	enum reac_disco_role role;
	const struct reac_box_model *model;
	/* The widest geometry heard from this peer; 0 while none was legal. Kept as a MAX rather
	 * than last-wins: a control frame carries no audio geometry, so a peer's data frames are
	 * what answer, and one stray short frame must not erase them. */
	unsigned channels;
	int owned;                 /* the peer THIS master established with */
	uint64_t first_seen_ns;
	uint64_t last_seen_ns;
};

struct reac_disco_table {
	struct reac_disco_entry e[REAC_DISCO_MAX];
	int n;
	int overflowed;            /* a MAC was dropped for lack of room: the list is partial */
	uint32_t seq;              /* bumped on every observable change; a frozen seq means
	                            * the publisher wedged — a reader must not read a stale
	                            * list as live truth. */
};

void reac_disco_table_init(struct reac_disco_table *t);

/* Record a sighting. Returns 1 when the table changed observably (new MAC, or a role/
 * model/ownership upgrade), 0 when it only refreshed a timestamp. Main thread only. */
int reac_disco_table_observe(struct reac_disco_table *t,
                             const struct reac_disco_sighting *s,
                             int owned, uint64_t now_ns);

/* Withdraw entries unheard for REAC_DISCO_STALE_NS. Returns how many were removed. */
int reac_disco_table_age(struct reac_disco_table *t, uint64_t now_ns);

/* Serialized age_ms is time since the last REPORTED sighting, not since the last frame:
 * the announce gate below collapses a live box's 8000 fps down to ~1 refresh/s, so a
 * healthy device reads 0..~1000 ms rather than ~0. That is deliberate (the ring must not
 * be flooded) and ample for a 5 s staleness bar — but it is a liveness indicator, never
 * a jitter measurement.
 *
 * Buffer the JSON snapshot must fit in. REAC_DISCO_MAX devices at ~110 B each, plus
 * slack — sized so a full table always serializes rather than being suppressed. */
#define REAC_DISCO_JSON_MAX 1280

/* Serialize to the JSON array published as reac.discovery.devices. One string is one
 * atomic snapshot — enumerated per-device prop keys would let a reader observe device 0
 * of the new set beside device 1 of the old. Returns the length written, or -1 when it
 * would not fit (the caller publishes nothing rather than a truncated list). */
int reac_disco_table_json(const struct reac_disco_table *t, uint64_t now_ns,
                          char *buf, size_t cap);

#endif /* REAC_DISCO_H */
