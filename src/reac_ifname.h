// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_ifname — a segment's STABLE, PHYSICAL-ADDRESS-DERIVED name: "pci<N>" / "usb<N>",
 * ported from openmixer's classifyBus (packages/server/src/net-interfaces.ts) with one
 * correction forced by measurement (see reac_ifname.c's header comment): a raw kernel
 * interface NAME (enp128s20f0u2, eth0, ...) is assigned by udev from bus topology or plain
 * enumeration order, and BOTH can change under the operator with the physical NIC untouched
 * (docs/NIC-PIN-BY-MAC.md documents the same instability for the name-pinning case this
 * addresses differently: derive an identity from the wire's own physical position, never
 * trust what udev happened to call it this boot).
 *
 * PURE OVER THE FILESYSTEM. reac_ifname_bus_addr reads exactly one resolved sysfs symlink
 * per call (no netlink, no socket) and is safe to call from the main thread on a scan tick;
 * the ranking/formatting halves touch no filesystem at all and are unit-testable with plain
 * string arrays. `root` overrides /sys/class/net in the filesystem-touching entry point, for
 * a test fixture. */
#ifndef REAC_IFNAME_H
#define REAC_IFNAME_H

#include <stddef.h>

enum reac_ifname_bus {
	REAC_IFNAME_BUS_OTHER = 0,   /* not PCI, not USB — a bridge, a virtual NIC, unresolved */
	REAC_IFNAME_BUS_PCI,
	REAC_IFNAME_BUS_USB,
};

/* Bound for the address string this module hands back ("0000:83:00.0", "4-2", ...). */
#define REAC_IFNAME_ADDR_MAX 48

/* Resolve `ifname`'s bus kind and physical bus address from
 * <root ?: "/sys/class/net">/<ifname>/device. `addr` is the USB PORT PATH ("4-2", the path
 * component immediately inside a "usbN" segment of the resolved path) for BUS_USB, or the
 * PCI slot address CLOSEST TO THE NIC ("0000:83:00.0", the LAST "DDDD:BB:DD.F"-shaped
 * component) for BUS_PCI — not a bridge's own address further up the chain, which would
 * fail to distinguish two NICs behind the same bridge. Returns 0 with *kind/addr filled
 * (BUS_OTHER leaves addr empty — a real fact, not a failure) on a resolvable device link,
 * -1 if the link itself could not be resolved (interface vanished mid-scan, no permission,
 * bad args) — never guesses. */
int reac_ifname_bus_addr(const char *root, const char *ifname,
                         enum reac_ifname_bus *kind, char *addr, size_t addr_cap);

/* 1-based rank of `self_addr` inside `addrs[0..n)`, sorted ascending by strcmp. Pure string
 * comparison, no filesystem: the SAME physical address always sorts to the SAME position for
 * a given candidate SET, independent of scan/enumeration order — which is what makes two
 * dongles "usb1"/"usb2" survive a reboot that re-orders which port udev names first.
 * KNOWN LIMIT (named, not hidden): the rank is relative to `addrs`, not persisted anywhere —
 * if a port drops out of the set entirely before a sibling is (re)ranked, the sibling's rank
 * can shift. Stable as long as the same SET of ports is present at ranking time, which is the
 * ordinary case (reboot/replug with nothing physically removed). Returns -1 if self_addr is
 * not present in addrs, or on bad args. */
int reac_ifname_rank(const char *const addrs[], int n, const char *self_addr);

/* Compose the final segment name: "pci<rank>" / "usb<rank>" for a resolved rank, or the raw
 * `ifname` unchanged when `kind` is BUS_OTHER or `rank` <= 0 (nothing to derive an index
 * from — never invents one). Returns the length written, or -1 if it would not fit `outlen`
 * or on bad args. */
int reac_ifname_derive(enum reac_ifname_bus kind, int rank, const char *ifname,
                       char *out, size_t outlen);

#endif /* REAC_IFNAME_H */
