// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_ifname — pinned against REAL captured /sys paths from a real box (2026-09-03), not
 * synthetic strings: enp128s20f0u2 (USB dongle) resolves to
 * /sys/devices/pci0000:80/0000:80:14.0/usb4/4-2/4-2:1.0, enp131s0 (onboard PCI) to
 * /sys/devices/pci0000:80/0000:80:1b.4/0000:83:00.0, wlp128s20f3 (PCI Wi-Fi) to
 * /sys/devices/pci0000:80/0000:80:14.3 — captured with `readlink -f`.
 *
 * The fixture replicates the REAL two-hop sysfs shape, not just the final target: on a real
 * kernel, `/sys/class/net/<ifname>` is ITSELF a symlink into `/sys/devices/<buspath>/net/
 * <ifname>`, and the `device` entry inside THAT directory is a symlink with a SHORT relative
 * target (`../../../<buspath's own leaf component>`) back up to the device's own directory —
 * never a target that spells out the full bus path. Building `<root>/<ifname>/device` as a
 * plain absolute-target symlink (this test's first draft) does not exercise that shape at
 * all: a single readlink() on an absolute-target symlink returns the very string a fix is
 * supposed to be needed to obtain, so the sabotage check (revert to a single readlink(), as
 * openmixer's classifyBus does) passed GREEN against the wrong fixture — caught only by
 * actually running the sabotage, per false-signals discipline. */
#include "reac_ifname.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* mkdir -p */
static void mkdirs(const char *path)
{
	char acc[1024] = "";
	for (const char *p = path; *p; ) {
		const char *n = strchr(p + 1, '/');
		size_t len = n ? (size_t)(n - path) : strlen(path);
		memcpy(acc, path, len);
		acc[len] = '\0';
		mkdir(acc, 0700);
		if (!n)
			break;
		p = n;
	}
}

/* Build the real two-hop shape for one NIC: `<root>/devices/<buspath>/net/<ifname>/device`,
 * a symlink whose target is the SHORT relative form the real kernel uses
 * ("../../../<basename(buspath)>", which always resolves back to
 * "<root>/devices/<buspath>" regardless of how deep buspath itself is — up 3 from
 * ".../<buspath>/net/<ifname>" lands at buspath's own parent, then back down into buspath's
 * own leaf component) — then `<root>/<ifname>` as the class/net-level symlink into that
 * directory, matching the real kernel's own indirection. */
static void link_device(const char *root, const char *ifname, const char *buspath)
{
	char netdir[1024];
	snprintf(netdir, sizeof netdir, "%s/devices/%s/net/%s", root, buspath, ifname);
	mkdirs(netdir);

	const char *leaf = strrchr(buspath, '/');
	leaf = leaf ? leaf + 1 : buspath;
	char devlink[1024], devtarget[128];
	snprintf(devlink, sizeof devlink, "%s/device", netdir);
	snprintf(devtarget, sizeof devtarget, "../../../%s", leaf);
	unlink(devlink);
	if (symlink(devtarget, devlink) != 0) {
		fprintf(stderr, "FAIL: symlink(%s -> %s): %m\n", devlink, devtarget);
		fails++;
	}

	char classlink[1024], classtarget[1024];
	snprintf(classlink, sizeof classlink, "%s/%s", root, ifname);
	snprintf(classtarget, sizeof classtarget, "%s", netdir);
	unlink(classlink);
	if (symlink(classtarget, classlink) != 0) {
		fprintf(stderr, "FAIL: symlink(%s -> %s): %m\n", classlink, classtarget);
		fails++;
	}
}

int main(void)
{
	char root[] = "/tmp/reac_ifname_test_XXXXXX";
	CHK(mkdtemp(root) != NULL);

	/* The real, captured bus paths (the part of `readlink -f .../device` after "devices/"). */
	link_device(root, "enp128s20f0u2", "pci0000:80/0000:80:14.0/usb4/4-2/4-2:1.0");
	link_device(root, "enp131s0", "pci0000:80/0000:80:1b.4/0000:83:00.0");
	link_device(root, "wlp128s20f3", "pci0000:80/0000:80:14.3");

	/* ---- reac_ifname_bus_addr: the real box's own three NICs. */
	enum reac_ifname_bus kind;
	char addr[REAC_IFNAME_ADDR_MAX];

	CHK(reac_ifname_bus_addr(root, "enp128s20f0u2", &kind, addr, sizeof addr) == 0);
	CHK(kind == REAC_IFNAME_BUS_USB);
	CHK(strcmp(addr, "4-2") == 0);

	CHK(reac_ifname_bus_addr(root, "enp131s0", &kind, addr, sizeof addr) == 0);
	CHK(kind == REAC_IFNAME_BUS_PCI);
	CHK(strcmp(addr, "0000:83:00.0") == 0);      /* the NIC's own slot, not the bridge's */

	CHK(reac_ifname_bus_addr(root, "wlp128s20f3", &kind, addr, sizeof addr) == 0);
	CHK(kind == REAC_IFNAME_BUS_PCI);
	CHK(strcmp(addr, "0000:80:14.3") == 0);

	/* An interface with no `device` link at all (vanished, or never existed): unresolved,
	 * never guessed. */
	CHK(reac_ifname_bus_addr(root, "ghost0", &kind, addr, sizeof addr) == -1);
	CHK(kind == REAC_IFNAME_BUS_OTHER && addr[0] == '\0');

	/* A resolvable device link that is neither PCI- nor USB-shaped (a virtual/bridge NIC):
	 * BUS_OTHER is a real fact, not a failure. */
	link_device(root, "lancluster0", "virtual/lancluster0");
	CHK(reac_ifname_bus_addr(root, "lancluster0", &kind, addr, sizeof addr) == 0);
	CHK(kind == REAC_IFNAME_BUS_OTHER && addr[0] == '\0');

	/* ---- reac_ifname_rank: sort order survives enumeration order, never renumbers a
	 * sibling whose own address did not change. */
	const char *usb_addrs_boot1[] = { "4-2", "4-9" };      /* first boot: udev named 4-2 first */
	const char *usb_addrs_boot2[] = { "4-9", "4-2" };      /* replug/reboot: order flipped */
	CHK(reac_ifname_rank(usb_addrs_boot1, 2, "4-2") == 1);
	CHK(reac_ifname_rank(usb_addrs_boot1, 2, "4-9") == 2);
	CHK(reac_ifname_rank(usb_addrs_boot2, 2, "4-2") == 1);   /* same rank, different array order */
	CHK(reac_ifname_rank(usb_addrs_boot2, 2, "4-9") == 2);
	CHK(reac_ifname_rank(usb_addrs_boot1, 2, "4-99") == -1);   /* not in the set */
	CHK(reac_ifname_rank(NULL, 0, "4-2") == -1);

	/* ---- reac_ifname_derive: the final name. */
	char out[64];
	CHK(reac_ifname_derive(REAC_IFNAME_BUS_USB, 1, "enp128s20f0u2", out, sizeof out) > 0);
	CHK(strcmp(out, "usb1") == 0);
	CHK(reac_ifname_derive(REAC_IFNAME_BUS_PCI, 2, "enp131s0", out, sizeof out) > 0);
	CHK(strcmp(out, "pci2") == 0);
	/* BUS_OTHER: nothing to index by — the raw name survives unchanged, never invented. */
	CHK(reac_ifname_derive(REAC_IFNAME_BUS_OTHER, 0, "lancluster0", out, sizeof out) > 0);
	CHK(strcmp(out, "lancluster0") == 0);
	/* A resolved kind with no rank (e.g. the interface was not in the ranking set): same
	 * refusal to invent an index. */
	CHK(reac_ifname_derive(REAC_IFNAME_BUS_PCI, 0, "enp131s0", out, sizeof out) > 0);
	CHK(strcmp(out, "enp131s0") == 0);
	/* Too small a buffer: refused, not truncated into a different, wrong name. */
	char tiny[3];
	CHK(reac_ifname_derive(REAC_IFNAME_BUS_USB, 1, "enp128s20f0u2", tiny, sizeof tiny) == -1);

	/* ---- END TO END: the composed name for this box's two real segments, as
	 * hearing_serve's call site would compute it — bus_addr + rank over the sibling set +
	 * derive, chained. */
	enum reac_ifname_bus k1, k2;
	char a1[REAC_IFNAME_ADDR_MAX], a2[REAC_IFNAME_ADDR_MAX];
	CHK(reac_ifname_bus_addr(root, "enp128s20f0u2", &k1, a1, sizeof a1) == 0);
	CHK(reac_ifname_bus_addr(root, "enp131s0", &k2, a2, sizeof a2) == 0);
	CHK(k1 == REAC_IFNAME_BUS_USB && k2 == REAC_IFNAME_BUS_PCI);
	const char *usb_set[] = { a1 };
	const char *pci_set[] = { a2 };
	int r1 = reac_ifname_rank(usb_set, 1, a1);
	int r2 = reac_ifname_rank(pci_set, 1, a2);
	CHK(reac_ifname_derive(k1, r1, "enp128s20f0u2", out, sizeof out) > 0);
	CHK(strcmp(out, "usb1") == 0);
	CHK(reac_ifname_derive(k2, r2, "enp131s0", out, sizeof out) > 0);
	CHK(strcmp(out, "pci1") == 0);

	if (fails) {
		fprintf(stderr, "FAILED: %d check(s)\n", fails);
		return 1;
	}
	printf("OK: reac_ifname — bus+address resolved from real captured /sys shapes "
	       "(realpath, not a single readlink), rank stable across enumeration order, "
	       "derive never invents an index it cannot resolve\n");
	return 0;
}
