// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* rx_bind_replay — what locking the slave side's receive gate to its desk changes, frame by
 * frame, over recorded sessions.
 *
 *   rx_bind_replay corpus <pcap>...     one line per capture; exit 1 on an unexplained difference
 *   rx_bind_replay attack <pcap>        a second host forges the desk's stream into a real capture
 *
 * Per capture, the hunt is run over the whole recording pinned slave (the state a slaving
 * daemon is in), and its answer is read the way main.c reads it: a SLAVE verdict against a
 * FOREIGN desk, with the rival's MAC, is the peer the gate is bound to (downstream). A box
 * on M is not bound (see hunt()); BOXLOCK=1 binds it too, upstream, to measure what that
 * would change. Then reac_rx's own feeder is run twice over
 * the recording, one pass per side:
 *
 *   main     the gate as main left it: never locked, so the first source it sees decides
 *   branch   the same gate after reac_rx_bind_hint, the call main.c now makes at open
 *
 * Each frame's outcome (decoded, not ours, duplicate, bad) is read off the feeder's counters
 * the moment it asks for the next frame, as libreac's peer_replay does (the reader is wrapped
 * at link time). A frame whose outcome differs between the sides is EXPLAINED only by:
 *
 *   foreign    main decoded it and the branch refused it, and its source is not the bound peer
 *   shadowed   the bound peer's own frame, which main refused and the branch decoded, within
 *              SHADOW frames after a foreign one main decoded (that frame moved main's
 *              counter, so the peer's next frames read as behind it)
 *
 * Anything else is red. A capture with no desk or box to bind runs one pass and is reported
 * as unbound: nothing can differ there, and a capture where nothing differs says so. Link
 * with -Wl,--wrap=pcap_source_next. */
#define _GNU_SOURCE
#include <reac/reac.h>
#include <reac/reac_hunt.h>
#include <reac/pcap_source.h>
#include <reac/transport/reac_ring.h>
#include <reac/transport/reac_rx.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/reac_rx_bind.h"

#define BUFSZ 4096
#define SHADOW 16
#define HUNT_POLL_NS (200ull * 1000000ull)

long __real_pcap_source_next(struct pcap_source *ps, uint8_t *buf, size_t cap, uint64_t *ts);

static size_t restore(uint8_t *buf, size_t cap, long n, uint32_t orig)
{
	if (n <= 0)
		return 0;
	size_t len = (size_t)n;
	if (orig > len && len >= 50 && orig <= cap) {
		memset(buf + len, 0, orig - len);
		len = orig;
		size_t clean = reac_frame_clean_len(len);
		if (clean >= 2) {
			buf[clean - 2] = REAC_END_MARKER_0;
			buf[clean - 1] = REAC_END_MARKER_1;
		}
	}
	return len;
}

/* ---- the observed feeder ---- */
struct rec { uint8_t out; uint8_t src[6]; uint16_t ctr; };
static struct {
	struct reac_rx *rx;
	struct rec *r;
	size_t n, cap;
	int have_prev;
	uint64_t ok, other, dup, bad;
	uint8_t src[6];
	uint16_t ctr;
} W;

static void settle(void)
{
	if (!W.have_prev)
		return;
	uint64_t ok = atomic_load(&W.rx->frames_ok), other = atomic_load(&W.rx->frames_other);
	uint64_t dup = atomic_load(&W.rx->frames_dup), bad = atomic_load(&W.rx->frames_bad);
	uint8_t out = 0;
	if (ok != W.ok) out = 1;
	else if (other != W.other) out = 2;
	else if (dup != W.dup) out = 3;
	else if (bad != W.bad) out = 4;
	W.ok = ok; W.other = other; W.dup = dup; W.bad = bad;
	if (W.n == W.cap) {
		W.cap = W.cap ? W.cap * 2 : 1 << 16;
		W.r = realloc(W.r, W.cap * sizeof *W.r);
		if (!W.r) exit(2);
	}
	W.r[W.n].out = out;
	memcpy(W.r[W.n].src, W.src, 6);
	W.r[W.n].ctr = W.ctr;
	W.n++;
	W.have_prev = 0;
}

long __wrap_pcap_source_next(struct pcap_source *ps, uint8_t *buf, size_t cap, uint64_t *ts)
{
	settle();
	if (!atomic_load(&W.rx->running))
		return 0;
	long n = __real_pcap_source_next(ps, buf, cap, ts);
	if (n <= 0) {
		atomic_store(&W.rx->running, 0);   /* one pass, then stop */
		return 0;
	}
	size_t len = restore(buf, cap, n, ps->last_orig_len);
	W.have_prev = 1;
	memset(W.src, 0, 6);
	if (len >= 16) {
		memcpy(W.src, buf + 6, 6);
		W.ctr = (uint16_t)(buf[14] | buf[15] << 8);
	}
	return (long)len;
}

/* One pass of the feeder over `pcap`; `bind` is the peer to lock to, NULL for main's gate. */
static void pass(const char *pcap, enum reac_rx_accept acc, const uint8_t *bind)
{
	static struct reac_rx rx;
	static struct reac_ring ring;
	struct reac_rx_cfg cfg = { .kind = REAC_RX_PCAP, .source = pcap, .forced_rate = 48000,
	                           .pcap_realtime = 0, .accept = acc };
	free(W.r);
	memset(&W, 0, sizeof W);
	W.rx = &rx;
	if (reac_rx_open(&rx, &cfg, &ring) != 0) {
		fprintf(stderr, "rx_bind_replay: cannot open %s\n", pcap);
		exit(2);
	}
	if (bind) {
		struct reac_rx_bind b;
		reac_rx_bind_init(&b);
		reac_rx_bind_hint(&b, &rx, bind);
	}
	if (reac_rx_start(&rx) != 0) exit(2);
	pthread_join(rx.thread, NULL);
	settle();
	reac_ring_free(&ring);
}

/* ---- the hunt, as the daemon would have it for this recording ---- */
struct hunted { int bind; enum reac_rx_accept acc; uint8_t mac[6]; const char *rival; };

static void hunt(const char *pcap, struct hunted *o)
{
	struct pcap_source ps;
	static const uint8_t us[6] = { 0x02, 0, 0, 0, 0, 0x01 };
	static struct reac_hunt h;
	static uint8_t buf[BUFSZ];
	uint64_t ts, next = 0;
	int init = 0;
	memset(o, 0, sizeof *o);
	o->rival = "none";
	if (pcap_source_open(&ps, pcap) != 0) exit(2);
	long n;
	while ((n = __real_pcap_source_next(&ps, buf, sizeof buf, &ts)) > 0) {
		size_t len = restore(buf, sizeof buf, n, ps.last_orig_len);
		uint64_t t = ts * 1000ull;
		if (!init) {
			reac_hunt_init(&h, us, t);
			reac_hunt_pin(&h, REAC_ROLE_SLAVE);
			next = t;
			init = 1;
		}
		while (next <= t) { reac_hunt_step(&h, next); next += HUNT_POLL_NS; }
		reac_hunt_observe(&h, buf, len, t, NULL);
	}
	pcap_source_close(&ps);
	if (!init) return;
	o->rival = reac_rival_kind_name(h.arb.rival);
	/* main.c binds a DESK only; a box on M (a join, received upstream) is left as it was,
	 * because locking it would start decoding its broadcast flood. `box_lock` is the
	 * measurement of that: BOXLOCK=1 in the environment binds boxes too, to show it. */
	int box_too = getenv("BOXLOCK") != NULL;
	if (h.verdict == REAC_HUNT_SLAVE && h.arb.state == REAC_SEGMENT_FOREIGN && h.arb.have_mac &&
	    (h.arb.rival == REAC_RIVAL_DESK || (box_too && h.arb.rival == REAC_RIVAL_BOX))) {
		o->bind = 1;
		memcpy(o->mac, h.arb.mac, 6);
		o->acc = h.arb.rival == REAC_RIVAL_BOX ? REAC_RX_ACCEPT_UPSTREAM
		                                       : REAC_RX_ACCEPT_DOWNSTREAM;
	}
}

static int corpus_one(const char *pcap)
{
	struct hunted hd;
	hunt(pcap, &hd);
	char ms[18] = "-";
	if (hd.bind)
		snprintf(ms, sizeof ms, "%02x:%02x:%02x:%02x:%02x:%02x", hd.mac[0], hd.mac[1],
		         hd.mac[2], hd.mac[3], hd.mac[4], hd.mac[5]);
	if (!hd.bind) {
		printf("%s unbound rival=%s\n", pcap, hd.rival);
		return 0;
	}
	pass(pcap, hd.acc, NULL);
	struct rec *a = W.r; size_t na = W.n;
	W.r = NULL; W.n = W.cap = 0;
	pass(pcap, hd.acc, hd.mac);
	struct rec *b = W.r; size_t nb = W.n;
	size_t same = 0, foreign = 0, shadowed = 0, bad = 0, last_foreign = (size_t)-1, bound_ok = 0;
	if (na != nb) {
		printf("%s %s FRAME COUNT DIFFERS main=%zu branch=%zu\n", pcap, ms, na, nb);
		free(a);
		return 1;
	}
	for (size_t i = 0; i < na; i++) {
		int mine = memcmp(a[i].src, hd.mac, 6) == 0;
		if (b[i].out == 1 && mine) bound_ok++;
		if (a[i].out == b[i].out) { same++; continue; }
		if (a[i].out == 1 && b[i].out == 2 && !mine) { foreign++; last_foreign = i; continue; }
		if (a[i].out == 2 && b[i].out == 1 && mine && last_foreign != (size_t)-1 &&
		    i - last_foreign <= SHADOW) { shadowed++; continue; }
		if (bad++ < 5)
			printf("  UNEXPLAINED frame %zu src=%02x%02x%02x%02x%02x%02x ctr=%u main=%u branch=%u\n", i,
			       a[i].src[0], a[i].src[1], a[i].src[2], a[i].src[3], a[i].src[4], a[i].src[5],
			       a[i].ctr, a[i].out, b[i].out);
	}
	printf("%s bound=%s %s frames=%zu same=%zu foreign_refused=%zu shadow_recovered=%zu "
	       "unexplained=%zu bound_peer_decoded=%zu\n", pcap, ms,
	       hd.acc == REAC_RX_ACCEPT_UPSTREAM ? "up" : "down", na, same, foreign, shadowed, bad,
	       bound_ok);
	free(a);
	return bad ? 1 : 0;
}

/* ---- the attack arm: a second host forges a real capture's desk stream ---- */
static void pcap_hdr(FILE *f)
{
	uint32_t gh[6] = { 0xa1b2c3d4, 0x00040002, 0, 0, 65535, 1 };
	fwrite(gh, sizeof gh, 1, f);
}

static int attack(const char *pcap)
{
	struct hunted hd;
	hunt(pcap, &hd);
	if (!hd.bind || hd.acc != REAC_RX_ACCEPT_DOWNSTREAM) {
		fprintf(stderr, "attack: %s names no desk to forge against\n", pcap);
		return 2;
	}
	/* The recording's own desk frames (the bound source, 1492 B broadcasts, up to 400),
	 * each followed by a copy from FORGER, which counts from a far offset and carries the
	 * desk's audio shifted to a tell-tale level. */
	static const uint8_t forger[6] = { 0x02, 0xde, 0xad, 0xbe, 0xef, 0x01 };
	char tmp[] = "/tmp/rxbind-attack-XXXXXX";
	int fd = mkstemp(tmp);
	FILE *f = fdopen(fd, "wb");
	pcap_hdr(f);
	struct pcap_source ps;
	static uint8_t buf[BUFSZ];
	uint64_t ts;
	long n;
	int desk = 0;
	if (pcap_source_open(&ps, pcap) != 0) return 2;
	while (desk < 400 && (n = __real_pcap_source_next(&ps, buf, sizeof buf, &ts)) > 0) {
		size_t len = restore(buf, sizeof buf, n, ps.last_orig_len);
		if (reac_frame_clean_len(len) != REAC_FRAME_BYTES || memcmp(buf + 6, hd.mac, 6) != 0 ||
		    memcmp(buf, "\xff\xff\xff\xff\xff\xff", 6) != 0)
			continue;
		uint32_t rh[4] = { 0, 0, (uint32_t)len, (uint32_t)len };
		fwrite(rh, sizeof rh, 1, f); fwrite(buf, len, 1, f);
		memcpy(buf + 6, forger, 6);
		uint16_t c = (uint16_t)((buf[14] | buf[15] << 8) + 30000);
		buf[14] = (uint8_t)c; buf[15] = (uint8_t)(c >> 8);
		fwrite(rh, sizeof rh, 1, f); fwrite(buf, len, 1, f);
		desk++;
	}
	pcap_source_close(&ps);
	fclose(f);
	if (desk < 50) { fprintf(stderr, "attack: only %d desk frames in %s\n", desk, pcap); return 2; }
	pass(tmp, REAC_RX_ACCEPT_DOWNSTREAM, NULL);
	size_t main_forged = 0, main_desk = 0;
	for (size_t i = 0; i < W.n; i++)
		if (W.r[i].out == 1) {
			if (memcmp(W.r[i].src, forger, 6) == 0) main_forged++; else main_desk++;
		}
	pass(tmp, REAC_RX_ACCEPT_DOWNSTREAM, hd.mac);
	size_t br_forged = 0, br_desk = 0;
	for (size_t i = 0; i < W.n; i++)
		if (W.r[i].out == 1) {
			if (memcmp(W.r[i].src, forger, 6) == 0) br_forged++; else br_desk++;
		}
	remove(tmp);
	printf("attack %s: %d desk frames each followed by a forged copy\n"
	       "  main:   decoded desk=%zu forged=%zu\n  branch: decoded desk=%zu forged=%zu\n",
	       pcap, desk, main_desk, main_forged, br_desk, br_forged);
	/* main must take the forger, the branch must not, and the branch must keep the desk */
	return (main_forged > 0 && br_forged == 0 && br_desk > 0) ? 0 : 1;
}

int main(int argc, char **argv)
{
	if (argc >= 3 && strcmp(argv[1], "attack") == 0)
		return attack(argv[2]);
	if (argc >= 3 && strcmp(argv[1], "corpus") == 0) {
		int rc = 0;
		for (int i = 2; i < argc; i++)
			rc |= corpus_one(argv[i]);
		return rc;
	}
	fprintf(stderr, "usage: rx_bind_replay corpus|attack <pcap>...\n");
	return 2;
}
