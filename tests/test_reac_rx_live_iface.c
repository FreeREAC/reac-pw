// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test: reac-pw must REFUSE a --live interface that does not exist,
 * loudly and at startup — never run deaf.
 *
 * Two things pinned here:
 *
 *   1. reac_rx_iface_present() — the primitive both reac_rx_open() and the
 *      feeder's mid-run alarm use to ask "does this name resolve to an
 *      interface at all?". No privilege needed (if_nametoindex() is a plain
 *      query), so this runs anywhere: "lo" always exists, a made-up name
 *      never does.
 *
 *   2. reac_rx_open(kind=LIVE, forced_rate=<set>) on a nonexistent interface
 *      must return -1. This is the actual regression: --rate is what the
 *      live systemd unit always passes (REAC_RATE=96000), and the interface
 *      validation used to be SKIPPED whenever forced_rate was set — a dead
 *      or renamed --live NIC opened "successfully" and only failed later,
 *      silently, inside the feeder thread. No sockets need to actually reach
 *      a REAC box for this: a made-up interface name is refused (ENODEV) the
 *      same way an absent CAP_NET_RAW is (EPERM) — reac_rx_open() must not
 *      tell those apart, it must refuse both.
 */
#include <stdio.h>
#include "reac_ring.h"
#include "reac_rx.h"

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

int main(void)
{
	CHK(reac_rx_iface_present("lo") == 1);
	CHK(reac_rx_iface_present("reacpw-test-does-not-exist-9182") == 0);
	CHK(reac_rx_iface_present(NULL) == 0);

	struct reac_rx_cfg cfg = { .kind = REAC_RX_LIVE,
	                           .source = "reacpw-test-does-not-exist-9182",
	                           .forced_rate = 96000,   /* the deployed --rate case */
	                           .accept = REAC_RX_ACCEPT_UPSTREAM };
	struct reac_ring ring;
	struct reac_rx rx;
	CHK(reac_rx_open(&rx, &cfg, &ring) == -1);

	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("OK: reac_rx_iface_present names real vs. absent interfaces; "
	       "reac_rx_open refuses a --live interface that does not exist even "
	       "when --rate forces the sample rate\n");
	return 0;
}
