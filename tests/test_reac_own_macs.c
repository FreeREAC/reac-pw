// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_own_macs — no address of this host is ever a rival (#8).
 *
 * The fixture is the mirrored port: the segment we master carries OUR master frames, sourced
 * from a SECOND interface of this host, not the NIC the sniffer listens on. The host's
 * interface table is a scripted netdir (`<name>/address`), read by the same code that reads
 * /sys/class/net, and the screen judges an Ethernet frame by its source address. The mirrored
 * stream itself, libreac's frames through the hunt with the screen in and out of the path,
 * is test_reac_hunt's: it crosses into libreac, which the perturbed-protocol check builds
 * against the real numbers. */
#include "reac_own_macs.h"

#include <net/ethernet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "reac_facts_pw.h"

static int fails;
#define CHK(c) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const uint8_t LISTEN[6] = { 0x34, 0x5a, 0x60, 0x9f, 0x9e, 0xbe };  /* the NIC the sniffer is on */
static const uint8_t SENDER[6] = { 0x34, 0x5a, 0x60, 0x9f, 0x9e, 0xc0 };  /* another NIC of this host */
static const uint8_t DESK[6]   = { 0x00, 0x40, 0xab, 0xc9, 0xcc, 0x03 };  /* a real M-200 */
static const uint8_t BCAST[6]  = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static void put_iface(const char *dir, const char *name, const char *addr)
{
	char p[512];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	mkdir(p, 0700);
	snprintf(p, sizeof p, "%s/%s/address", dir, name);
	FILE *f = fopen(p, "w");
	if (f) {
		fprintf(f, "%s\n", addr);
		fclose(f);
	}
}

static void rm_iface(const char *dir, const char *name)
{
	char p[512];
	snprintf(p, sizeof p, "%s/%s/address", dir, name);
	unlink(p);
	snprintf(p, sizeof p, "%s/%s", dir, name);
	rmdir(p);
}

int main(void)
{
	char dir[] = "/tmp/reac-own-macs-XXXXXX";
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}
	put_iface(dir, "lo", "00:00:00:00:00:00");
	put_iface(dir, "enp1s0", "34:5a:60:9f:9e:be");
	put_iface(dir, "enp2s0", "34:5a:60:9f:9e:c0");

	struct reac_own_macs own;
	memset(&own, 0, sizeof own);
	int nread = reac_own_macs_read(&own, dir);

	/* ---- A. THE SET IS THE HOST'S INTERFACE TABLE, every interface, loopback's zero apart. */
	CHK(nread == 2);
	CHK(own.n == 2);
	CHK(reac_own_macs_has(&own, LISTEN) == 1);
	CHK(reac_own_macs_has(&own, SENDER) == 1);
	CHK(reac_own_macs_has(&own, DESK) == 0);
	CHK(reac_own_macs_read(&own, "/nonexistent/net") == -1);
	CHK(own.n == 2);

	/* ---- B. THE SCREEN READS THE ETHERNET SOURCE: a frame from either of our interfaces is
	 * dropped, said once, and a desk's is passed on. (The hunt this keeps the stream out of
	 * runs in test_reac_hunt, whose frames are libreac's.) */
	uint8_t fr[64] = { 0 };
	struct ether_header *eh = (struct ether_header *)fr;
	memcpy(eh->ether_dhost, BCAST, 6);
	int said = 0;
	memcpy(eh->ether_shost, SENDER, 6);
	CHK(reac_own_echo_screen(&own, &said, fr, sizeof fr) == REAC_OWN_ECHO_FIRST);
	said = 1;
	CHK(reac_own_echo_screen(&own, &said, fr, sizeof fr) == REAC_OWN_ECHO_AGAIN);
	memcpy(eh->ether_shost, LISTEN, 6);
	CHK(reac_own_echo_screen(&own, &said, fr, sizeof fr) == REAC_OWN_ECHO_AGAIN);
	said = 0;
	memcpy(eh->ether_shost, DESK, 6);
	CHK(reac_own_echo_screen(&own, &said, fr, sizeof fr) == REAC_OWN_ECHO_NONE);
	CHK(said == 0);

	/* ---- C. A SHORT FRAME has no source to judge and is passed on. */
	uint8_t shorty[8] = { 0 };
	CHK(reac_own_echo_screen(&own, &said, shorty, sizeof shorty) == REAC_OWN_ECHO_NONE);

	/* ---- D. THE TABLE IS RE-READ when interfaces change: a NIC that appears is ours from
	 * then on, one that is removed no longer is. */
	static const uint8_t LATE[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
	put_iface(dir, "usb0", "02:11:22:33:44:55");
	CHK(reac_own_macs_has(&own, LATE) == 0);
	CHK(reac_own_macs_read(&own, dir) == 3);
	CHK(reac_own_macs_has(&own, LATE) == 1);
	rm_iface(dir, "usb0");
	CHK(reac_own_macs_read(&own, dir) == 2);
	CHK(reac_own_macs_has(&own, LATE) == 0);

	rm_iface(dir, "lo");
	rm_iface(dir, "enp1s0");
	rm_iface(dir, "enp2s0");
	rmdir(dir);
	if (fails)
		return 1;
	printf("ok: own addresses are never a rival\n");
	return 0;
}
