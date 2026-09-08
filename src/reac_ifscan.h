/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * reac_ifscan — WHICH interfaces to listen on, and which of them are SEGMENTS. The host's
 * netdev table, watched over rtnetlink, folded into one decision per interface
 * (openmixer's docs/design/specs/2026-08-23-reac-trunk-vlan-daemon.md §7-§9, amendment
 * 2026-09-02: every linked WIRED interface is listened on; nothing is declared per NIC).
 *
 * LINK IS THE GATE TO LISTEN; HEARING IS THE GATE TO SERVE. An Ethernet interface that carries
 * link (IFF_LOWER_UP) is SNIFFED — a passive 0x8819 socket that transmits nothing, so an office
 * LAN costs one idle descriptor. The first frame that classifies as REAC gear turns the interface
 * into a SEGMENT, and only then does the full listener (FSM, ring, nodes, a master that
 * transmits) open on it. A NIC that has link and never carries REAC is never a segment, never a
 * node, never a row.
 *
 * WI-FI IS EXCLUDED BY DEFAULT, OPT-IN ONLY. `ifi_type == ARPHRD_ETHER` is true of a wireless
 * NIC too (found 2026-09-03: it was passing this table's gate with no exclusion at all, not
 * even a deny-list), and REAC's timing has no tolerance for Wi-Fi's jitter — this project has
 * no repacer to absorb it (DESIGN.md's ring-depth note: "enough to swallow a WiFi tail spike"
 * is about a WIRED ring's margin, not a claim Wi-Fi itself works). A wireless interface
 * (`/sys/class/net/<if>/wireless` or `/phy80211` exists — reac_ifscan_is_wireless) never
 * reaches this table's `ether` gate unless its name is listed in REAC_IFACES_ALLOW_WIRELESS
 * (comma-separated, or "*" for all — reac_ifscan_wireless_allowed). Unset/empty = every
 * wireless NIC excluded, which is the default and the common case.
 *
 * A SEGMENT DROPS ON LINK LOSS, WITH HYSTERESIS. Carrier gone starts a hold; carrier back inside
 * it cancels the hold and the segment keeps its FSM, ring and nodes — a box power-cycle, a PHY
 * renegotiation and a re-seated plug are all shorter than the hold and must not churn the graph.
 * Carrier still gone at the end of the hold drops the segment, and the interface goes back to
 * being sniffed so the next box on that port is heard afresh. RTM_DELLINK drops at once: a
 * netdev that is gone has nothing to hold for. A flap is COUNTED so a chattering port is a
 * number the operator can read, not a mystery.
 *
 * THE NAME IS THE SEGMENT. §5 rules a segment IS an interface, so its identity is the
 * interface's name and nothing in a file. An interface that comes back under the same name with
 * a new ifindex (a USB NIC re-enumerating) is treated as gone-then-new: its segment drops and is
 * heard again, loudly, never followed in silence — the 2026-08-29 outage was a daemon that kept
 * a binding to an ifindex that no longer existed.
 *
 * PURE CORE, NETLINK SHELL. The table and its transitions take observations and a clock and
 * emit EVENTS; the caller (main.c) owns the sockets and the listeners and does what the events
 * say. The netlink half is only a byte source for the core, and the unit test feeds it the same
 * synthetic datagrams reac_linkmon's test feeds that module. NOT RT-SAFE: main loop only.
 */
#ifndef REAC_IFSCAN_H
#define REAC_IFSCAN_H

#include <net/if.h>   /* IFNAMSIZ */
#include <stddef.h>
#include <stdint.h>

/* Bounded, like the listener array it feeds: a host with more Ethernet interfaces than this
 * has the excess REPORTED at observation time, never silently untracked. */
#define REAC_IFSCAN_MAX 16

/* How long carrier may be gone before a segment is dropped. Longer than a box power-cycle's
 * link bounce (~1-2 s measured on the rig's boxes) and a PHY autonegotiation restart (<3 s);
 * shorter than an operator walking to the other end of the room. */
#define REAC_IFSCAN_DOWN_HOLD_NS (3ULL * 1000000000ULL)

/* After a listener REFUSED to open on a heard segment (the segment lock is held by another
 * process, the socket could not be bound), how long the interface's sightings are ignored
 * before another attempt — otherwise the next frame retries at wire speed. */
#define REAC_IFSCAN_RETRY_NS (5ULL * 1000000000ULL)

enum reac_ifscan_state {
	REAC_IFSCAN_ABSENT = 0,  /* not in the table (an entry in this state is free) */
	REAC_IFSCAN_DOWN,        /* Ethernet, known, no carrier: nothing to listen to */
	REAC_IFSCAN_LINKED,      /* carrier up, sniffed, no REAC heard */
	REAC_IFSCAN_SEGMENT,     /* REAC heard: the full listener is (being) served */
	REAC_IFSCAN_HOLD,        /* a segment whose carrier is gone, inside the hold */
};

const char *reac_ifscan_state_name(enum reac_ifscan_state s);

struct reac_ifscan_entry {
	char name[IFNAMSIZ];
	int  ifindex;
	enum reac_ifscan_state state;
	uint64_t hold_until_ns;   /* HOLD: when the drop falls due */
	uint64_t retry_after_ns;  /* LINKED: sightings ignored until then (0 = none) */
	unsigned flaps;           /* carrier lost and back inside the hold, for the operator */
};

/* What the caller must DO. Delivered in order through reac_ifscan_next(); every event names
 * the interface, and the caller owns every socket and listener the verbs refer to. */
enum reac_ifscan_verb {
	REAC_IFSCAN_NONE = 0,
	REAC_IFSCAN_LISTEN,   /* carrier came: open the passive sniffer */
	REAC_IFSCAN_UNLISTEN, /* carrier went or the netdev did: close the sniffer */
	REAC_IFSCAN_SERVE,    /* REAC heard: close the sniffer, open the listener */
	REAC_IFSCAN_DROP,     /* hold expired, netdev gone or re-enumerated: close the listener */
	REAC_IFSCAN_KEPT,     /* carrier back inside the hold: nothing to do, worth a line */
};

const char *reac_ifscan_verb_name(enum reac_ifscan_verb v);

struct reac_ifscan_event {
	enum reac_ifscan_verb verb;
	char name[IFNAMSIZ];
};

/* The event queue is bounded to what one observation burst can produce: a dump reply lists
 * every interface once, and each yields at most two events. */
#define REAC_IFSCAN_EVENTS 64

struct reac_ifscan {
	struct reac_ifscan_entry ifs[REAC_IFSCAN_MAX];
	struct reac_ifscan_event ev[REAC_IFSCAN_EVENTS];
	int ev_head, ev_tail;
	int fd;                   /* AF_NETLINK/NETLINK_ROUTE, RTNLGRP_LINK; -1 = not open */
	unsigned long msgs;       /* RTM_NEWLINK/RTM_DELLINK accepted; zero after a known link
	                           * change means the watch is not working */
	unsigned long overruns;   /* ENOBUFS: the cache is suspect and gets re-dumped */
	unsigned long unbounded;  /* interfaces that could not be tracked (table full) */
	unsigned long dropped_ev; /* events that could not be queued (never expected) */
};

/* ---- the pure core ---------------------------------------------------------------------- */

void reac_ifscan_init(struct reac_ifscan *s);

/* One observation of an interface, from a dump or a change. `ether` is ARPHRD_ETHER and not
 * loopback — anything else is ignored outright (a wireguard tunnel, lo, a bridge's own
 * netdev never become segments). A name already known under a DIFFERENT ifindex is a
 * re-enumeration: gone, then new. */
void reac_ifscan_observe(struct reac_ifscan *s, const char *name, int ifindex, int ether,
                         int lower_up, uint64_t now_ns);

/* RTM_DELLINK: the netdev left. A segment drops at once; a sniffed interface stops. */
void reac_ifscan_gone(struct reac_ifscan *s, const char *name, int ifindex, uint64_t now_ns);

/* The sniffer on `name` classified a REAC frame. LINKED -> SEGMENT, SERVE queued. Any other
 * state (or inside the retry window) ignores it. */
void reac_ifscan_heard(struct reac_ifscan *s, const char *name, uint64_t now_ns);

/* The listener could not be opened on a heard segment: back to LINKED with a retry window,
 * so the caller re-opens the sniffer (LISTEN is queued) and the next attempt waits. */
void reac_ifscan_serve_failed(struct reac_ifscan *s, const char *name, uint64_t now_ns);

/* Advance the clock: every HOLD past its due time drops. Call from a periodic timer. */
void reac_ifscan_tick(struct reac_ifscan *s, uint64_t now_ns);

/* Pop the next event; returns 0 when the queue is empty. */
int reac_ifscan_next(struct reac_ifscan *s, struct reac_ifscan_event *out);

/* The entry for `name`, or NULL. */
const struct reac_ifscan_entry *reac_ifscan_find(const struct reac_ifscan *s, const char *name);

/* Number of entries in `state`. */
int reac_ifscan_count(const struct reac_ifscan *s, enum reac_ifscan_state state);

/* Parse one netlink datagram, folding every RTM_NEWLINK/RTM_DELLINK it carries through
 * observe/gone. A truncated or malformed buffer is dropped, never guessed. */
void reac_ifscan_feed(struct reac_ifscan *s, const void *buf, size_t len, uint64_t now_ns);

/* ---- wireless exclusion (opt-in only) ---------------------------------------------------- */

/* `<root ?: "/sys/class/net">/<ifname>/wireless` or `/phy80211` exists: both are kernel-
 * guaranteed markers of a wireless NIC (checked either, since which one a given driver
 * populates varies) and neither implies the other's absence proves anything — the check
 * is "does either exist", not "do both". `root` overrides /sys/class/net for a test
 * fixture; NULL = the real filesystem. Never fails loud: an unreadable/nonexistent path
 * reads as "not wireless" (0), same as any other interface this table has never heard of. */
int reac_ifscan_is_wireless(const char *root, const char *ifname);

/* Does `allowlist` (REAC_IFACES_ALLOW_WIRELESS's value; NULL/empty = nothing) opt `ifname`
 * in? "*" opts in every wireless NIC; otherwise a comma-separated list of exact interface
 * names. Pure string matching, no I/O. */
int reac_ifscan_wireless_allowed(const char *allowlist, const char *ifname);

/* ---- the netlink shell ------------------------------------------------------------------ */

/* Open the watch (RTNLGRP_LINK, unprivileged) and seed the table with an RTM_GETLINK dump
 * through the same parser every later change uses. Returns 0, or -1 with the struct left
 * inert (fd -1; the pure calls above still work, nothing arrives by itself). */
int reac_ifscan_open(struct reac_ifscan *s, uint64_t now_ns);

/* The pollable descriptor, or -1. */
int reac_ifscan_fd(const struct reac_ifscan *s);

/* Read the socket dry, folding everything in. On ENOBUFS the table is re-dumped rather than
 * trusted. Never blocks. */
void reac_ifscan_drain(struct reac_ifscan *s, uint64_t now_ns);

/* Ask the kernel for the whole table again (RTM_GETLINK dump), through the same parser. */
int reac_ifscan_resync(struct reac_ifscan *s, uint64_t now_ns);

void reac_ifscan_close(struct reac_ifscan *s);

#endif /* REAC_IFSCAN_H */
