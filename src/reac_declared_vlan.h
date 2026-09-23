/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/* reac_declared_vlan — the VLAN segments the OPERATOR declared, as opposed to the ones
 * the wire happened to mention.
 *
 * WHAT A DECLARATION IS, SINCE 2026-09-23 (operator: "we don't carry any VLANs if we don't
 * detect VLANs"; segments spec amendment of that date). A `[segment <parent>.<vid>]` section
 * in `~/.config/reac-pw/reac-pw.conf` names a VLAN IF IT EXISTS: its role and ignore apply
 * the moment that VID is heard on the parent and the heard table (reac_topo, main.c's
 * topo_ensure) mints it by the same rule as any other tag — a tagged frame of any ethertype
 * names the VID (2026-09-22). A declaration mints NOTHING by itself, and a minted VLAN's
 * lifetime is the heard table's: released when silent, removed on exit.
 *
 * WHAT IT WAS, AND WHY IT IS NOT. From 2026-09-15 a declared VLAN was minted at start and
 * whenever its parent appeared, because a cold stagebox is a SLAVE that says nothing until
 * a master speaks, and the master cannot speak until the netdev exists. Two later rulings
 * made that unnecessary — a cold trunk names its VIDs through every tagged frame it carries,
 * and a box waiting on a PHY edge is woken by the wake ladder — and the desk paid for it
 * every boot: three VLANs minted on a parent that hears no tag, three vacant tap doors,
 * three roster rows, re-created after every drop of the parent
 * (docs/design/evidence/reac-pw-boot-2026-09-23.log).
 *
 * WHAT COUNTS AS A DECLARATION, SINCE 2026-09-16. A `[segment <parent>.<vid>]` section —
 * reac_segconf_declared is the one reader, and this module is the table it fills. Naming
 * the segment at all is the declaration; role and ignore are separate questions about it.
 * Before that, any conf KEY whose name ended in `_<parent>.<vid>` (typically
 * `REAC_ROLE_enp131s0.11`) declared one as a SIDE EFFECT of a role projection the console
 * generated, and outlived what declared it (auto-role §5d). */
#ifndef REAC_DECLARED_VLAN_H
#define REAC_DECLARED_VLAN_H

#include <net/if.h>
#include <stddef.h>
#include <stdint.h>

/* Two trunks' worth at reac_topo's own per-parent bound. Anything past it is REPORTED by
 * the caller, never silently dropped. */
#define REAC_DECLARED_VLAN_MAX 32

struct reac_declared_vlan {
	char     parent[IFNAMSIZ];
	uint16_t vid;
};

/* Split a segment name into parent and VID at its LAST dot. Returns 1 when it is a VLAN
 * segment, 0 when it is not (no dot, a non-numeric or out-of-range tail, an empty parent,
 * a name that would not fit). A VID is 1..4094: 0 is the priority tag and 4095 is
 * reserved, and neither names a netdev anyone can create. */
int reac_declared_vlan_split(const char *segment, char *parent, size_t cap, uint16_t *vid);

/* Add (parent, vid) to `tab` if it is not already there. Returns 1 added, 0 duplicate,
 * -1 table full. `*n` is the live count and is advanced on an add. */
int reac_declared_vlan_add(struct reac_declared_vlan *tab, int max, int *n,
                           const char *parent, uint16_t vid);

#endif /* REAC_DECLARED_VLAN_H */
