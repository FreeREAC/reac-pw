// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test: reac-pw must REFUSE a --live interface that does not exist,
 * loudly and at startup — never run deaf — and must NOTICE when the interface
 * it is bound to is replaced underneath it.
 *
 * Three things pinned here:
 *
 *   1. reac_rx_iface_present() — the primitive reac_rx_open() uses to ask
 *      "does this name resolve to an interface at all?". No privilege needed
 *      (if_nametoindex() is a plain query), so this runs anywhere: "lo"
 *      always exists, a made-up name never does.
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
 *
 *   3. reac_rx_binding_lost() — the MID-RUN predicate, and the one that was
 *      wrong. An AF_PACKET socket is bound to an IFINDEX; the name is only
 *      how we found that index once, at open.
 *
 *      2026-08-29, the S-0808 segment: the AX88179 carrying it was unplugged
 *      at 22:14:20 and re-registered at 22:17:00 under the SAME name and the
 *      SAME MAC (enp128s20f0u6 / 00:14:5c:9b:28:2d) with a NEW ifindex. The
 *      old alarm asked "does the name resolve?" — so it fired for the 2.5 min
 *      the name was absent, then went SILENT the moment the name came back,
 *      while the socket stayed bound to the dead index: deaf (rx_box_frames
 *      frozen at 4452241) and mute (tx frozen, tx_errors climbing ~4000/s).
 *      For four more minutes the daemon's only message was "still PROBING ...
 *      bounce the box PHY", which sent the operator to replug a box that was
 *      never at fault and could not have helped — a REAC box only cold-
 *      connects on link-up, and there was no master on the wire to answer.
 *
 *      A NAME RESOLVING IS NOT THE BINDING SURVIVING. Compare the indices.
 */
#include <stdio.h>
#include <reac/transport/reac_ring.h>
#include <reac/transport/reac_rx.h>

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

int main(void)
{
	CHK(reac_rx_iface_present("lo") == 1);
	CHK(reac_rx_iface_present("reacpw-test-does-not-exist-9182") == 0);
	CHK(reac_rx_iface_present(NULL) == 0);

	/* The index primitive the alarm actually needs. "lo" is 1 on every Linux
	 * in practice, but assert only what is guaranteed: a real name resolves
	 * to NON-ZERO, an absent one to 0 — the same sentinel if_nametoindex()
	 * uses for "no such interface", which is why 0 can never be a legitimate
	 * bound index. */
	unsigned lo = reac_rx_iface_index("lo");
	CHK(lo != 0);
	CHK(reac_rx_iface_index("reacpw-test-does-not-exist-9182") == 0);
	CHK(reac_rx_iface_index(NULL) == 0);

	/* The 2026-08-29 fault, stated as three cases. (b) is the one the
	 * name-only alarm missed and the one that cost the night. */
	CHK(reac_rx_binding_lost(lo, lo) == 0);  /* (a) unchanged      -> healthy */
	CHK(reac_rx_binding_lost(lo, 0)  == 1);  /* (c) name gone      -> lost    */
	CHK(reac_rx_binding_lost(6, 9)   == 1);  /* (b) SAME NAME, NEW INDEX:
	                                          *     the USB re-enumeration
	                                          *     -> lost                   */

	/* No baseline means nothing to compare: a listener that never learned its
	 * index must not be killed by the alarm. */
	CHK(reac_rx_binding_lost(0, lo) == 0);
	CHK(reac_rx_binding_lost(0, 0)  == 0);

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
	       "when --rate forces the sample rate; reac_rx_binding_lost catches a "
	       "SAME-NAME re-enumeration, not just a vanished name\n");
	return 0;
}
