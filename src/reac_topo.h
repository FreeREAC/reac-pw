/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * reac_topo — is this NIC an access port or a TRUNK, and which VLANs carry REAC on it.
 * (openmixer's docs/design/specs/2026-08-23-reac-trunk-vlan-daemon.md §3, §4, §5.)
 *
 * OBSERVE ONCE TO LEARN THE TOPOLOGY; NEVER PARSE A TAG IN THE AUDIO PATH. Learning that
 * tagged 0x8819 frames arrive, and which VIDs they carry, is a read-only discovery act;
 * stripping a tag off every frame of a paced audio path is the kernel's job and never
 * ours. So this module answers ONE question per parent — which VLAN ids were heard — and
 * the answer is turned into `<parent>.<vid>` netdevs (reac_vlan.h) that the ordinary
 * hearing path then serves like any other interface. Nothing here touches audio.
 *
 * THE TAG IS METADATA, NOT BYTES, AND THAT DECIDES THE SOCKET. Measured on this kernel
 * (7.2.4-200.fc44, 2026-09-09, openmixer's tools/probe-vlan-8819.py in a private netns,
 * matching the spec's §3 table taken on 6.x):
 *
 *   - a socket bound to 0x8819 ON THE PARENT receives every tagged frame as well, with the
 *     802.1Q header ABSENT from the buffer and `vlan_tci = none` — indistinguishable from
 *     an untagged frame. A detector built on that socket reports every trunk as an access
 *     port, and the parent's own sniffer really is hearing another VLAN's box;
 *   - an ETH_P_ALL tap on the parent sees the same frame with PACKET_AUXDATA
 *     `tp_vlan_tci = 111` and TP_STATUS_VLAN_VALID set. The kernel already decided which
 *     VLAN the frame came from; we ask it rather than deciding again;
 *   - a VID for which NO sub-interface exists still reaches the parent and the tap can
 *     still NAME it (vid 222 with no netdev). That is what makes an unconfigured VLAN
 *     visible, and it is the whole basis of the detector.
 *
 * Hence: ONE ETH_P_ALL SOCKET PER PHYSICAL PARENT, BPF-filtered to 0x8819, read-only,
 * never transmitting. The filter matters — without it an ETH_P_ALL socket on a trunk
 * copies every frame on the link to userspace. It accepts the accelerated case (the tag in
 * metadata, ethertype 0x8819 at offset 12) and the in-buffer case (0x8100/0x88a8 at 12,
 * 0x8819 behind it) alike, because whether a driver strips the tag is a driver's business
 * and a classifier that assumed one of them would go deaf on the other.
 *
 * THE DETECTOR ONLY EVER UPGRADES (§4b). There is no moment at which it must conclude "this
 * is not a trunk": untagged 0x8819 with no sub-interfaces is an ordinary segment and is
 * today's behaviour; any tagged 0x8819 means a VLAN exists and needs a segment. So no dwell,
 * no window to tune. A box that first speaks on VID 12 an hour into the show is served an
 * hour into the show and nothing was concluded wrongly in the meantime.
 *
 * A PARENT THAT CARRIES TAGGED REAC IS NEVER ITSELF DRIVEN (§3's ruling, §4f). It receives
 * every sub-interface's frames untagged (the measurement above), so driving it would put a
 * master on the parent while masters run on its sub-interfaces — two masters for one box,
 * arrived at through a kernel behaviour rather than a second process. `reac_topo_is_trunk`
 * is what the hearing path asks before serving.
 *
 * PURE CORE, SOCKET SHELL, like reac_ifscan: the table takes classifications and a clock
 * and emits ENSURE/RELEASE events; the caller owns the netdevs and the sockets. NOT
 * RT-SAFE: main loop only.
 */
#ifndef REAC_TOPO_H
#define REAC_TOPO_H

#include <net/if.h>   /* IFNAMSIZ */
#include <stddef.h>
#include <stdint.h>

/* Physical parents watched at once, and VLANs tracked per parent. The link budget caps a
 * gigabit trunk at about twenty segments at 48 kHz and eight recommended at 96 kHz (§12a),
 * so 16 VIDs per parent is past the wire's own limit; anything beyond either bound is
 * REPORTED (`unbounded`), never silently untracked. */
#define REAC_TOPO_MAX_PARENTS 8
#define REAC_TOPO_MAX_VLANS   16

/* A VID heard once and never again is a box that was unplugged, a switch reconfigured, or
 * one stray frame. Its netdev is removed after this long without a frame — but only if we
 * minted it. Comfortably longer than the segment hold (3 s) and than any box power-cycle,
 * because removing a netdev drops a segment and re-creating it is not free. */
#define REAC_TOPO_SILENCE_HOLD_NS (30ULL * 1000000000ULL)

/* After an ensure FAILED (no CAP_NET_ADMIN, the name is taken by something that is not a
 * VLAN device, the kernel refused), how long before another attempt. Without it every
 * frame on that VID retries at wire speed. */
#define REAC_TOPO_RETRY_NS (10ULL * 1000000000ULL)

/* What one frame on a parent's tap turned out to be. */
enum reac_topo_kind {
	REAC_TOPO_NOT_REAC = 0, /* not 0x8819 — the BPF should have dropped it */
	REAC_TOPO_UNTAGGED,     /* 0x8819, no VLAN: the parent itself may be the segment */
	REAC_TOPO_TAGGED,       /* 0x8819 inside a tag: a segment on VLAN <vid> of this parent */
};

const char *reac_topo_kind_name(enum reac_topo_kind k);

/* THE CLASSIFIER, pure. `buf`/`len` as read from the tap; `tci_valid` is
 * (tp_status & TP_STATUS_VLAN_VALID) and `tci` is tp_vlan_tci from PACKET_AUXDATA — the
 * kernel's own answer, zero and invalid when it has none.
 *
 * An accelerated tag OUTRANKS an in-buffer one: on a QinQ frame the kernel strips the
 * outer tag into metadata and the inner stays in the bytes, and it is the OUTER VID that
 * names the netdev a frame arrives on. VID 0 is a priority tag and names no VLAN (802.1Q),
 * so it reads as UNTAGGED and never mints `<parent>.0`.
 *
 * `vid_out` may be NULL. Returns the kind; a short or malformed buffer is NOT_REAC,
 * never a guess. */
enum reac_topo_kind reac_topo_classify(const void *buf, size_t len, int tci_valid,
                                       uint16_t tci, uint16_t *vid_out);

/* ---- the table -------------------------------------------------------------------- */

enum reac_topo_vstate {
	REAC_TOPO_VLAN_FREE = 0,
	REAC_TOPO_VLAN_HEARD,   /* frames seen, the netdev is not ensured yet */
	REAC_TOPO_VLAN_SERVED,  /* `<parent>.<vid>` exists and is ours to keep alive */
};

enum reac_topo_verb {
	REAC_TOPO_NONE = 0,
	REAC_TOPO_ENSURE,   /* adopt `<parent>.<vid>` if it exists, else create it, and bring it up */
	REAC_TOPO_RELEASE,  /* the VID went silent past the hold, or the parent went away */
};

const char *reac_topo_verb_name(enum reac_topo_verb v);

struct reac_topo_event {
	enum reac_topo_verb verb;
	char parent[IFNAMSIZ];
	uint16_t vid;
	int minted;   /* RELEASE only: was this netdev ours to delete */
};

#define REAC_TOPO_EVENTS 32

struct reac_topo_vlan {
	uint16_t vid;
	enum reac_topo_vstate state;
	uint64_t last_ns;        /* when a frame on this VID was last classified */
	uint64_t retry_after_ns; /* HEARD: no ensure attempt before then (0 = none) */
	unsigned long frames;
	int minted;              /* WE created this netdev, so we delete it. Adopted = 0 */
};

struct reac_topo_parent {
	char name[IFNAMSIZ];     /* "" = free slot */
	struct reac_topo_vlan v[REAC_TOPO_MAX_VLANS];
	unsigned long tagged;    /* tagged 0x8819 frames classified on this parent */
	unsigned long untagged;  /* untagged ones — §4f's "both" case is visible as both > 0 */
	int said_untagged;       /* the "untagged REAC on a trunk is not served" line, said once */
};

struct reac_topo {
	struct reac_topo_parent p[REAC_TOPO_MAX_PARENTS];
	struct reac_topo_event ev[REAC_TOPO_EVENTS];
	int ev_head, ev_tail;
	unsigned long unbounded;  /* parents or VIDs that did not fit, for the operator */
	unsigned long dropped_ev;
};

void reac_topo_init(struct reac_topo *t);

/* Start/stop watching a physical parent. `watch` is idempotent; `unwatch` queues a RELEASE
 * for every netdev we minted on it (the parent is gone or lost carrier, so its
 * sub-interfaces carry nothing) and frees the slot. Returns 0/-1 (table full, counted). */
int reac_topo_watch(struct reac_topo *t, const char *parent);
void reac_topo_unwatch(struct reac_topo *t, const char *parent, uint64_t now_ns);

/* One classified frame. TAGGED with a fresh VID queues ENSURE; a VID already served just
 * refreshes its clock. UNTAGGED only counts — the parent is served by the ordinary
 * hearing path, and refused there once this parent is a trunk. */
void reac_topo_saw(struct reac_topo *t, const char *parent, enum reac_topo_kind kind,
                   uint16_t vid, uint64_t now_ns);

/* The caller carried out an ENSURE: the netdev is up, and `minted` says whether we created
 * it (1) or adopted one that was already there (0). §4d: what we minted, we remove; what we
 * adopted survives us, untouched. */
void reac_topo_ensured(struct reac_topo *t, const char *parent, uint16_t vid, int minted);

/* The ENSURE could not be carried out. Stays HEARD, with a retry window, so a daemon
 * without CAP_NET_ADMIN reports and goes on listening rather than spinning. */
void reac_topo_ensure_failed(struct reac_topo *t, const char *parent, uint16_t vid,
                             uint64_t now_ns);

/* Advance the clock: every SERVED VID silent past REAC_TOPO_SILENCE_HOLD_NS is released. */
void reac_topo_tick(struct reac_topo *t, uint64_t now_ns);

/* Every netdev we still hold, released — the clean-exit path of §4d. */
void reac_topo_release_all(struct reac_topo *t);

int reac_topo_next(struct reac_topo *t, struct reac_topo_event *out);

const struct reac_topo_parent *reac_topo_find(const struct reac_topo *t, const char *parent);
const struct reac_topo_vlan *reac_topo_vlan_find(const struct reac_topo *t, const char *parent,
                                                 uint16_t vid);

/* Has tagged REAC been heard on this parent? A trunk parent is never itself driven. An
 * unknown parent is not a trunk (0): silence is never evidence of a topology. */
int reac_topo_is_trunk(const struct reac_topo *t, const char *parent);

/* Untagged REAC heard on a parent that is ALSO carrying tagged REAC — §4f's one refusal,
 * "REAC on a trunk's native VLAN is not served; give it a tag". Returns 1 the FIRST time it
 * is true for this parent, so the caller says it once and not per frame. */
int reac_topo_untagged_on_trunk(struct reac_topo *t, const char *parent);

/* VIDs on this parent in `state` (REAC_TOPO_VLAN_FREE counts nothing). */
int reac_topo_count(const struct reac_topo *t, const char *parent, enum reac_topo_vstate st);

/* Is `ifname` STACKED on another netdev — a VLAN sub-interface, a bridge, a bond master —
 * rather than a physical parent? Reads `<root ?: "/sys/class/net">/<ifname>/` for a
 * `lower_*` entry, which the kernel creates for every device built on another one; a
 * physical NIC has none. A stacked device is never tapped: a VLAN sub-interface has no
 * VLANs of its own, and tapping it would find the very tag the kernel just stripped.
 * `root` overrides /sys/class/net for a test fixture. Never fails loud — an unreadable or
 * absent path reads as not stacked (0), the same answer as any ordinary NIC. */
int reac_topo_is_stacked(const char *root, const char *ifname);

/* ---- the socket shell ------------------------------------------------------------- */

/* An ETH_P_ALL tap on `parent`, BPF-filtered to 0x8819 (tagged or not) and asking the
 * kernel for PACKET_AUXDATA. Read-only: nothing is ever sent on it. Returns the fd, or -1
 * with errno set (CAP_NET_RAW missing, the interface gone). Non-blocking. */
int reac_topo_tap_open(const char *parent);

/* Read ONE frame from the tap and classify it. Returns 1 with `*kind`/`*vid` filled, 0 when
 * the socket is dry (EAGAIN), -1 on error. The VID comes from PACKET_AUXDATA when the kernel
 * supplies it, from the buffer when the driver left the tag in it. */
int reac_topo_tap_next(int fd, enum reac_topo_kind *kind, uint16_t *vid);

void reac_topo_tap_close(int fd);

#endif /* REAC_TOPO_H */
