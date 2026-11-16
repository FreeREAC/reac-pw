// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_own_macs — no address of this host is ever a rival (#8).
 *
 * The fixture is the mirrored port: the segment we master carries OUR master frames, sourced
 * from a SECOND interface of this host, not the NIC the sniffer listens on. The host's
 * interface table is a scripted netdir (`<name>/address`), read by the same code that reads
 * /sys/class/net. The control is the same stream with the screen out of the path: it must
 * read as a foreign desk, or the fixture proves nothing.
 *
 * Frames are libreac's builders, never hand-rolled. */
#include "reac_own_macs.h"

#include <reac/reac_hunt.h>
#include <reac/reac_arbitration.h>
#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_master.h>
#include <reac/reac_encode.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "reac_facts_pw.h"

static int fails;
#define CHK(c) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SEC 1000000000ULL
#define STREAM_COPIES 10
#define STREAM_GAP_NS (3ULL * 1000000ULL)

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

/* A desk's master stream as a mirrored port delivers it: a head-amp record and the cfea
 * announce, STREAM_COPIES times, from `src`. Each frame goes through the screen first,
 * exactly as the sniffer's drain does; `scr` NULL leaves the screen out. Returns how many
 * frames the screen dropped. */
static int stream_from(struct reac_hunt *h, const struct reac_own_macs *scr, int *said,
                       const uint8_t src[6], uint64_t now, int *firsts)
{
	static struct reac_master m;
	reac_master_init(&m, src, NULL, REAC_PKT_RATE_96K);
	int dropped = 0;
	for (int k = 0; k < STREAM_COPIES; k++) {
		uint64_t t = now + (uint64_t)k * STREAM_GAP_NS;
		uint8_t f[2048];
		size_t n = reac_ctrl_build_headamp(f, BCAST, src, 0x30, REACPW_S1608_HEADAMP_BASE,
		                                   REAC_HEADAMP_PARAM_PHANTOM, 1);
		float *none[1] = { NULL };
		uint8_t g[2048];
		size_t gn = (size_t)reac_downstream_build(g, none, 0, REAC_SAMPLES_PER_PKT, 7, src);
		if (n == 0 || gn == 0 || reac_master_stamp(&m, g, REAC_M_EMIT_ANNOUNCE, 0) != 0)
			return -1;
		const uint8_t *fr[2] = { f, g };
		size_t ln[2] = { n, gn };
		for (int i = 0; i < 2; i++) {
			if (scr) {
				enum reac_own_echo e = reac_own_echo_screen(scr, said, fr[i], ln[i]);
				if (e != REAC_OWN_ECHO_NONE) {
					dropped++;
					if (e == REAC_OWN_ECHO_FIRST && firsts)
						(*firsts)++;
					continue;
				}
			}
			reac_hunt_observe(h, fr[i], ln[i], t, NULL);
		}
	}
	return dropped;
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

	uint64_t t0 = 100 * SEC;
	struct reac_hunt h;

	/* ---- B. CONTROL: with the screen out of the path our own stream, sourced from the
	 * second interface, is a foreign desk. This is the defect. */
	reac_hunt_init(&h, LISTEN, t0);
	CHK(stream_from(&h, NULL, NULL, SENDER, t0, NULL) == 0);
	reac_hunt_step(&h, t0 + SEC / 10);
	CHK(h.arb.state == REAC_SEGMENT_FOREIGN);
	CHK(h.arb.rival == REAC_RIVAL_DESK);
	CHK(h.verdict == REAC_HUNT_SLAVE);

	/* ---- C. THE FIX: the same stream through the screen never reaches the hunt, and the
	 * window closes on a wire nobody else spoke on: we stay master. */
	reac_hunt_init(&h, LISTEN, t0);
	int said = 0, firsts = 0;
	CHK(stream_from(&h, &own, &said, SENDER, t0, &firsts) == 2 * STREAM_COPIES);
	CHK(firsts == 1);
	CHK(said == 1);
	CHK(h.arb.state != REAC_SEGMENT_FOREIGN);
	CHK(h.arb.rival != REAC_RIVAL_DESK);
	CHK(reac_hunt_step(&h, t0 + REAC_HUNT_WINDOW_NS + SEC) >= 0);
	CHK(h.verdict != REAC_HUNT_SLAVE);
	CHK(reac_hunt_role(&h) == REAC_ROLE_MASTER);

	/* ---- D. THE LISTENING NIC'S OWN ADDRESS is dropped too. */
	reac_hunt_init(&h, LISTEN, t0);
	said = 0; firsts = 0;
	CHK(stream_from(&h, &own, &said, LISTEN, t0, &firsts) == 2 * STREAM_COPIES);
	CHK(firsts == 1);

	/* ---- E. A REAL DESK STILL IS ONE: the screen drops our addresses and nothing else. */
	reac_hunt_init(&h, LISTEN, t0);
	said = 0; firsts = 0;
	CHK(stream_from(&h, &own, &said, DESK, t0, &firsts) == 0);
	CHK(firsts == 0 && said == 0);
	reac_hunt_step(&h, t0 + SEC / 10);
	CHK(h.arb.rival == REAC_RIVAL_DESK);
	CHK(h.verdict == REAC_HUNT_SLAVE);
	CHK(memcmp(h.arb.mac, DESK, 6) == 0);

	/* ---- F. A SHORT FRAME has no source to judge and is passed on. */
	uint8_t shorty[8] = { 0 };
	CHK(reac_own_echo_screen(&own, &said, shorty, sizeof shorty) == REAC_OWN_ECHO_NONE);

	/* ---- G. THE TABLE IS RE-READ when interfaces change: a NIC that appears is ours from
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
