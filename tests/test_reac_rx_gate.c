// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test: the RX stream gate (role-driven downstream/upstream accept).
 *
 * Writes a temp pcap that interleaves the two REAC streams a real wire
 * carries — the master's 1492 B / 40-ch downstream broadcast and a box's
 * 628 B / 16-ch braided upstream return (a REAL sanitized captured frame) —
 * plus a second box's return, then runs the actual reac_rx feeder thread
 * over it in both accept modes and checks:
 *
 *   DOWNSTREAM: only the 1492 B frames feed the ring (upstream -> frames_other),
 *               with their BRAIDED audio decoded onto the channel that wrote it
 *               and its pair partner left silent
 *   UPSTREAM:   only the FIRST box's return feeds the ring (downstream + the
 *               second box -> frames_other), with its braided audio decoded
 *               into ring channels 0..15 and 16..39 silent.
 *
 * No sockets, no PipeWire — pcap replay flat-out, unit scope.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

#include <reac/reac.h>
#include <reac/transport/reac_ring.h>
#include <reac/transport/reac_rx.h>
#include <reac/reac_braid.h>
#include <reac/reac_upstream.h>
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

#include "upstream_fixtures.inc"

static int fails;
#define CHK(cond) do { \
	if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* ---- minimal classic-pcap writer (LE, linktype 1) ---- */
static void pcap_hdr(FILE *f)
{
	uint32_t gh[6] = { 0xa1b2c3d4, 0x00040002, 0, 0, 65535, 1 };
	fwrite(gh, sizeof gh, 1, f);
}

static void pcap_rec(FILE *f, const uint8_t *frame, uint32_t len)
{
	uint32_t rh[4] = { 0, 0, len, len };
	fwrite(rh, sizeof rh, 1, f);
	fwrite(frame, len, 1, f);
}

/* a well-formed synthetic 40-ch downstream broadcast frame */
static void mk_downstream(uint8_t *out, uint16_t counter)
{
	memset(out, 0, REAC_FRAME_BYTES);
	/* dst broadcast, src the master stand-in MAC */
	memset(out, 0xff, 6);
	static const uint8_t master[6] = { 0x00, 0x40, 0xab, 0xc4, 0x91, 0x90 };
	memcpy(out + 6, master, 6);
	out[12] = 0x88; out[13] = 0x19;
	out[14] = (uint8_t)(counter & 0xff); out[15] = (uint8_t)(counter >> 8);
	/* Audio region: a marker value on channel 0 and silence everywhere else,
	 * laid down through the braid oracle — the wire layout the master really
	 * emits and, since libreac 0.5.0, the one reac_decode() reads back. This
	 * fixture used to write the marker at the plain-LE sample-major offset
	 * (s*40+ch)*3, which pinned the pre-0.5.0 decode: under the braid those
	 * same bytes land on channel 1's high lane, so the frame no longer says
	 * what it claims to (#80). */
	for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
		size_t pos[3];
		reac_braid_pos(s, 0, 40, pos);
		uint8_t *audio = out + REAC_L2_HEADER_LEN;
		audio[pos[0]] = 0x00;                  /* s24 0x400000 = +0.5 */
		audio[pos[1]] = 0x00;
		audio[pos[2]] = 0x40;
	}
	out[REAC_FRAME_BYTES - 2] = REAC_END_MARKER_0;
	out[REAC_FRAME_BYTES - 1] = REAC_END_MARKER_1;
}

/* An OHRCA (M-5000/M-480) downstream frame: the standard 1492 B frame plus 2
 * further bytes AFTER the C2 EA end marker (total 1494 B). What those bytes are
 * is open — a real per-frame trailer per libreac's <reac/reac.h>, or Ethernet
 * FCS bytes leaked in by a mirror/SPAN tap per docs/SLAVE-EMULATION-SCOPE.md
 * W4(a); see #80. This test does not care, and neither does the code under it:
 * the gate must accept the 1494 B length and the decoder must read the embedded
 * 1492 B frame, ignoring whatever follows. `out` must have room for
 * REAC_FRAME_BYTES_OHRCA. */
static void mk_downstream_ohrca(uint8_t *out, uint16_t counter)
{
	mk_downstream(out, counter);                 /* fills [0 : REAC_FRAME_BYTES) */
	out[REAC_FRAME_BYTES + 0] = 0xB1;            /* two arbitrary post-marker */
	out[REAC_FRAME_BYTES + 1] = 0x06;            /* bytes — never decoded */
}

/* run the feeder over the fixture until it has accepted n frames (or timeout) */
static int run_rx(struct reac_rx *rx, uint64_t want_ok)
{
	if (reac_rx_start(rx) != 0)
		return -1;
	for (int i = 0; i < 2000; i++) { /* <= 2 s */
		if (atomic_load(&rx->frames_ok) >= want_ok)
			break;
		struct timespec ts = { 0, 1000000 };
		nanosleep(&ts, NULL);
	}
	reac_rx_stop(rx);
	return atomic_load(&rx->frames_ok) >= want_ok ? 0 : -1;
}

int main(void)
{
	/* ---- build the mixed-stream fixture ---- */
	char path[] = "/tmp/reacpw-gate-XXXXXX";
	int fd = mkstemp(path);
	CHK(fd >= 0);
	FILE *f = fdopen(fd, "wb");
	CHK(f != NULL);
	pcap_hdr(f);

	uint8_t down[REAC_FRAME_BYTES];
	uint8_t up2[sizeof UP16];
	memcpy(up2, UP16, sizeof up2);
	up2[11] = 0x01; /* a SECOND box: different src MAC tail */
	for (uint16_t i = 0; i < 40; i++) {
		mk_downstream(down, i);
		pcap_rec(f, down, sizeof down);
		pcap_rec(f, UP16, sizeof UP16);   /* box 1 (the sanitized capture) */
		pcap_rec(f, up2, sizeof up2);     /* box 2: must be gated out */
	}
	fclose(f);

	/* the expected float of box-1 channel 0, sample 0 (braided s24 -> f32) */
	uint8_t pcm[16 * REAC_SAMPLES_PER_PKT * 3];
	CHK(reac_upstream_decode(UP16, sizeof UP16, pcm) == REAC_SAMPLES_PER_PKT);

	/* ---- DOWNSTREAM accept: only the 1492 B broadcast feeds the ring ---- */
	{
		struct reac_rx_cfg cfg = { .kind = REAC_RX_PCAP, .source = path,
		                           .forced_rate = REAC_SAMPLE_RATE_48K, .pcap_realtime = 0,
		                           .accept = REAC_RX_ACCEPT_DOWNSTREAM };
		struct reac_ring ring;
		struct reac_rx rx;
		CHK(reac_rx_open(&rx, &cfg, &ring) == 0);
		CHK(run_rx(&rx, 20) == 0);
		uint64_t ok = atomic_load(&rx.frames_ok);
		CHK(ok >= 20);
		/* both boxes' returns gated out: the pcap interleaves down,up1,up2, so at
		 * least 2 gated frames per accepted one (minus the in-flight triplet) */
		CHK(atomic_load(&rx.frames_other) >= 2 * (ok - 1));
		CHK(atomic_load(&rx.frames_bad) == 0);

		/* ring channel 0 carries the downstream marker +0.5, and its braid
		 * PARTNER (ch 1, the other half of the pair) is silent — the pair is
		 * where a plain-LE read of a braided frame goes wrong, so this is the
		 * layout pin, not just a liveness check */
		float ch[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
		float *dst[REAC_MAX_CHANNELS];
		for (int c = 0; c < REAC_MAX_CHANNELS; c++) dst[c] = ch[c];
		CHK(reac_ring_read_planar(&ring, dst, REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT) == REAC_SAMPLES_PER_PKT);
		CHK(fabsf(ch[0][0] - 0.5f) < 1e-6f);
		CHK(ch[1][0] == 0.0f);
		reac_rx_close(&rx);
		reac_ring_free(&ring);
	}

	/* ---- THE DECLARED BOX WINS OVER THE ONE THAT SPEAKS FIRST ----
	 * The gate latches the first box-shaped source it sees, and nothing used to
	 * unlatch it. After a hot swap that left it decoding a MAC which had left the
	 * segment: every frame from the new box failed the compare, frames_ok froze,
	 * and reac-capture published silence while the wire carried a live microphone
	 * (rig 2026-08-21, S-1608 out / S-0808 in). Box identity belongs to the
	 * master, so it can say so — and then box 1 speaking first must not win. */
	{
		struct reac_rx_cfg cfg = { .kind = REAC_RX_PCAP, .source = path,
		                           .forced_rate = REAC_SAMPLE_RATE_48K, .pcap_realtime = 0,
		                           .accept = REAC_RX_ACCEPT_UPSTREAM };
		struct reac_ring ring;
		struct reac_rx rx;
		CHK(reac_rx_open(&rx, &cfg, &ring) == 0);
		reac_rx_peer_reset(&rx, up2 + 6, 1);          /* the master declares BOX 2 */
		CHK(rx.up_src_locked == 1);
		CHK(run_rx(&rx, 20) == 0);
		/* box 2's frames fed the ring; box 1 — first on the wire — was gated out */
		CHK(memcmp(rx.up_src, up2 + 6, 6) == 0);
		CHK(memcmp(rx.up_src, UP16 + 6, 6) != 0);
		CHK(atomic_load(&rx.frames_ok) >= 20);
		CHK(atomic_load(&rx.frames_bad) == 0);
		/* idempotent: the same box in the same session changes nothing */
		unsigned ep = atomic_load(&rx.peer_epoch);
		reac_rx_peer_reset(&rx, up2 + 6, 1);
		CHK(memcmp(rx.up_src, up2 + 6, 6) == 0);
		CHK(atomic_load(&rx.peer_epoch) == ep);
		/* WARM RECONNECT: the SAME box comes back, but it is a NEW SESSION and its
		 * counter starts wherever the box's did. The MAC cannot tell this apart,
		 * so keying on it alone left the old session's continuity in place and a
		 * clean reconnect reported thousands of counter gaps. The session must
		 * bump the epoch even though nothing about the peer changed. */
		reac_rx_peer_reset(&rx, up2 + 6, 2);
		CHK(atomic_load(&rx.peer_epoch) == ep + 1);
		CHK(memcmp(rx.up_src, up2 + 6, 6) == 0);   /* still the same box */
		reac_rx_close(&rx);
		reac_ring_free(&ring);
	}

	/* ---- UPSTREAM accept: only box 1's return feeds the ring ---- */
	{
		struct reac_rx_cfg cfg = { .kind = REAC_RX_PCAP, .source = path,
		                           .forced_rate = REAC_SAMPLE_RATE_48K, .pcap_realtime = 0,
		                           .accept = REAC_RX_ACCEPT_UPSTREAM };
		struct reac_ring ring;
		struct reac_rx rx;
		CHK(reac_rx_open(&rx, &cfg, &ring) == 0);
		CHK(run_rx(&rx, 20) == 0);
		uint64_t ok = atomic_load(&rx.frames_ok);
		CHK(ok >= 20);
		/* the downstream frames AND the second box got gated out */
		CHK(atomic_load(&rx.frames_other) >= 2 * (ok - 1));
		CHK(atomic_load(&rx.frames_bad) == 0);
		CHK(rx.up_src_locked == 1);
		CHK(memcmp(rx.up_src, UP16 + 6, 6) == 0);

		/* ring channels 0..15 carry box 1's braided audio; 16..39 are silent */
		float ch[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
		float *dst[REAC_MAX_CHANNELS];
		for (int c = 0; c < REAC_MAX_CHANNELS; c++) dst[c] = ch[c];
		CHK(reac_ring_read_planar(&ring, dst, REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT) == REAC_SAMPLES_PER_PKT);
		int bad = 0;
		for (int c = 0; c < 16; c++)
			for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++) {
				const uint8_t *p = &pcm[(size_t)(c * REAC_SAMPLES_PER_PKT + s) * 3];
				int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
				                      ((uint32_t)p[2] << 16));
				if (v & 0x00800000) v |= ~0x00FFFFFF;
				if (fabsf(ch[c][s] - (float)v / 8388608.0f) > 1e-7f)
					bad++;
			}
		CHK(bad == 0);
		for (int c = 16; c < REAC_MAX_CHANNELS; c++)
			for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
				if (ch[c][s] != 0.0f)
					bad++;
		CHK(bad == 0);
		reac_rx_close(&rx);
		reac_ring_free(&ring);
	}

	unlink(path);

	/* ---- OHRCA 1494 B downstream: accepted AND decoded (trailer ignored) ---- */
	{
		char opath[] = "/tmp/reacpw-gate-ohrca-XXXXXX";
		int ofd = mkstemp(opath);
		CHK(ofd >= 0);
		FILE *of = fdopen(ofd, "wb");
		CHK(of != NULL);
		pcap_hdr(of);
		uint8_t odown[REAC_FRAME_BYTES_OHRCA];
		for (uint16_t i = 0; i < 40; i++) {
			mk_downstream_ohrca(odown, i);
			pcap_rec(of, odown, sizeof odown);   /* 1494 B OHRCA frames only */
		}
		fclose(of);

		struct reac_rx_cfg cfg = { .kind = REAC_RX_PCAP, .source = opath,
		                           .forced_rate = REAC_SAMPLE_RATE_96K, .pcap_realtime = 0,
		                           .accept = REAC_RX_ACCEPT_DOWNSTREAM };
		struct reac_ring ring;
		struct reac_rx rx;
		CHK(reac_rx_open(&rx, &cfg, &ring) == 0);
		CHK(run_rx(&rx, 20) == 0);
		CHK(atomic_load(&rx.frames_ok) >= 20);   /* 1494 B frames accepted */
		CHK(atomic_load(&rx.frames_bad) == 0);   /* none rejected as malformed */

		/* the embedded 1492 B frame decoded: channel 0 still carries +0.5 and
		 * its braid partner is still silent — the 2 extra bytes reached no lane */
		float ch[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
		float *dst[REAC_MAX_CHANNELS];
		for (int c = 0; c < REAC_MAX_CHANNELS; c++) dst[c] = ch[c];
		CHK(reac_ring_read_planar(&ring, dst, REAC_MAX_CHANNELS, REAC_SAMPLES_PER_PKT) == REAC_SAMPLES_PER_PKT);
		CHK(fabsf(ch[0][0] - 0.5f) < 1e-6f);
		CHK(ch[1][0] == 0.0f);
		reac_rx_close(&rx);
		reac_ring_free(&ring);
		unlink(opath);
	}

	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("OK: rx gate — downstream accept feeds only the 1492 B broadcast; upstream "
	       "accept locks to the first box, decodes its braid into ch 0..15, silences "
	       "the rest, gates out the downstream + a second box\n");
	return 0;
}
