// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_ifname — see reac_ifname.h.
 *
 * WHY realpath(), NOT A SINGLE readlink() — openmixer's classifyBus (this module's starting
 * point) does the latter (`readlinkSync(devicePath)`, checking the result for "/usb"/"/pci").
 * Measured on a real box, 2026-09-03: `readlink("/sys/class/net/enp128s20f0u2/device")`
 * returns `../../../4-2:1.0` — a string that contains NEITHER substring. `/sys/class/net/
 * <ifname>` is ITSELF a symlink (to `.../devices/<bus-path>/net/<ifname>`), so a single
 * readlink() on the `device` entry inside it only ever sees the LAST hop, never the bus path
 * classifyBus greps for; the same measurement on `enp131s0` (PCI) and `wlp128s20f3` (PCI,
 * wireless) gave `../../../0000:83:00.0` and `../../../0000:80:14.3` — same shape, same
 * miss. openmixer's own test fixtures fabricate the symlink TARGET string directly
 * (`symlinkSync('/some/usb/path', ...)`), which never exercises a real kernel one-hop link
 * and so could not have caught this (false-signals #2: a self-consistent suite proves the
 * suite, not the filesystem). realpath() asks the kernel to walk the WHOLE chain and hands
 * back the canonical absolute path, which does carry the bus path
 * ("/sys/devices/pci0000:80/0000:80:14.0/usb4/4-2/4-2:1.0") — confirmed against this box's
 * real NICs; see tests/test_reac_ifname.c. */
#include "reac_ifname.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DDDD:BB:DD.F — 4 hex, ':', 2 hex, ':', 2 hex, '.', 1+ hex, nothing after. The exact PCI
 * BDF shape, not a loose digit scan, so an unrelated component that merely contains digits
 * (a USB port path, a driver's own directory) never matches. */
static int is_pci_bdf(const char *s)
{
	size_t i = 0;
	int n;

	n = 0; while (isxdigit((unsigned char)s[i])) { i++; n++; }
	if (n != 4 || s[i] != ':') return 0;
	i++;
	n = 0; while (isxdigit((unsigned char)s[i])) { i++; n++; }
	if (n != 2 || s[i] != ':') return 0;
	i++;
	n = 0; while (isxdigit((unsigned char)s[i])) { i++; n++; }
	if (n != 2 || s[i] != '.') return 0;
	i++;
	n = 0; while (isxdigit((unsigned char)s[i])) { i++; n++; }
	return n >= 1 && s[i] == '\0';
}

/* "usb" followed by one or more digits, nothing else — the kernel's own naming for a USB
 * host controller's root hub directory in sysfs (usb1, usb2, usb4, ...). */
static int is_usbN(const char *s)
{
	if (strncmp(s, "usb", 3) != 0 || !s[3])
		return 0;
	for (const char *p = s + 3; *p; p++)
		if (!isdigit((unsigned char)*p))
			return 0;
	return 1;
}

int reac_ifname_bus_addr(const char *root, const char *ifname,
                         enum reac_ifname_bus *kind, char *addr, size_t addr_cap)
{
	if (kind)
		*kind = REAC_IFNAME_BUS_OTHER;
	if (addr && addr_cap)
		addr[0] = '\0';
	if (!ifname || !ifname[0] || !kind || !addr || addr_cap == 0)
		return -1;
	if (!root || !root[0])
		root = "/sys/class/net";

	char path[PATH_MAX];
	int n = snprintf(path, sizeof path, "%s/%s/device", root, ifname);
	if (n < 0 || (size_t)n >= sizeof path)
		return -1;

	char resolved[PATH_MAX];
	if (!realpath(path, resolved))
		return -1;   /* vanished mid-scan, or no such link: unresolved, never guessed */

	/* Walk the resolved path's components once. A USB match ("usbN") wins immediately — the
	 * component right after it is the port path. A PCI match keeps the LAST BDF-shaped
	 * component seen: the one closest to the NIC, past any bridge on the way there. */
	char buf[PATH_MAX];
	snprintf(buf, sizeof buf, "%s", resolved);
	char pci_addr[REAC_IFNAME_ADDR_MAX];
	pci_addr[0] = '\0';

	char *save = NULL;
	char *comp = strtok_r(buf, "/", &save);
	while (comp) {
		if (is_usbN(comp)) {
			char *next = strtok_r(NULL, "/", &save);
			if (next) {
				*kind = REAC_IFNAME_BUS_USB;
				snprintf(addr, addr_cap, "%s", next);
				return 0;
			}
			break;   /* a usbN with nothing after it: not a usable port path */
		}
		if (is_pci_bdf(comp))
			snprintf(pci_addr, sizeof pci_addr, "%s", comp);
		comp = strtok_r(NULL, "/", &save);
	}
	if (pci_addr[0]) {
		*kind = REAC_IFNAME_BUS_PCI;
		snprintf(addr, addr_cap, "%s", pci_addr);
	}
	return 0;   /* resolved; BUS_OTHER (addr left empty) is a real fact when neither matched */
}

int reac_ifname_rank(const char *const addrs[], int n, const char *self_addr)
{
	if (!addrs || n <= 0 || !self_addr || !self_addr[0])
		return -1;
	int found = 0;
	for (int i = 0; i < n; i++)
		if (addrs[i] && strcmp(addrs[i], self_addr) == 0) { found = 1; break; }
	if (!found)
		return -1;
	int rank = 1;
	for (int i = 0; i < n; i++)
		if (addrs[i] && strcmp(addrs[i], self_addr) < 0)
			rank++;
	return rank;
}

int reac_ifname_derive(enum reac_ifname_bus kind, int rank, const char *ifname,
                       char *out, size_t outlen)
{
	if (!ifname || !out || outlen == 0)
		return -1;
	const char *prefix = kind == REAC_IFNAME_BUS_USB ? "usb"
	                    : kind == REAC_IFNAME_BUS_PCI ? "pci" : NULL;
	int w = (prefix && rank > 0) ? snprintf(out, outlen, "%s%d", prefix, rank)
	                             : snprintf(out, outlen, "%s", ifname);
	if (w < 0 || (size_t)w >= outlen)
		return -1;
	return w;
}
