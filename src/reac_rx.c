// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  /* clock_nanosleep / TIMER_ABSTIME */
#endif
#include "reac_rx.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <net/if.h>   /* if_nametoindex — resolving the --live name to an index */
#include <netpacket/packet.h>  /* sockaddr_ll — the index the socket is BOUND to */

#include <reac/reac.h>
/* the downstream (40-ch braided) decode + the two wire sources, reused as-is */
#include <reac/reac_decode.h>
#include <reac/reac_capture.h>
#include <reac/pcap_source.h>
/* the box-return (braided, box-width) decode — the master-role RX path */
#include <reac/reac_upstream.h>
/* reac_s24le_to_f32 — the one conversion pair (exact inverse of the TX side) */
#include <reac/reac_sample.h>

static uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

unsigned reac_rx_iface_index(const char *ifname)
{
	return ifname ? if_nametoindex(ifname) : 0;
}

int reac_rx_iface_present(const char *ifname)
{
	return reac_rx_iface_index(ifname) != 0;
}

int reac_rx_binding_lost(unsigned bound_ifindex, unsigned current_ifindex)
{
	if (bound_ifindex == 0)
		return 0;   /* no baseline learned yet: nothing to compare */
	return current_ifindex != bound_ifindex;
}

/* The ifindex an open AF_PACKET socket is bound to, straight from the socket
 * (getsockname fills sockaddr_ll.sll_ifindex), or 0 if it cannot be read.
 *
 * Deliberately NOT if_nametoindex(name) a second time: that would re-derive the
 * baseline from the same name we are about to test it against, so a NIC that
 * re-enumerated between open and this call would be recorded as its own new
 * index and the mismatch could never be seen. Ask the socket what it is bound
 * to; that is the fact the alarm is about. */
static unsigned capture_bound_ifindex(int fd)
{
	struct sockaddr_ll sll;
	socklen_t len = sizeof sll;
	if (getsockname(fd, (struct sockaddr *)&sll, &len) != 0)
		return 0;
	return (unsigned)sll.sll_ifindex;
}

/* Decode one gate-accepted frame into the ring (planar float, ring-width x 12
 * samples). Both directions are the SAME channel-pair braid, read through
 * libreac's one oracle: DOWNSTREAM = the 40-ch broadcast (reac_decode, braided
 * since libreac 0.5.0 — before that it read plain LE while our encoder wrote
 * the braid, #80); UPSTREAM = the box-width return (reac_upstream_decode),
 * placed positionally at ring channels 0..nch-1 with the remaining slots silent
 * (the input->slot allocation is a separate lane). */
static void feed_frame(struct reac_rx *rx, const struct reac_mode *mode,
                       const uint8_t *frame, size_t len)
{
	uint8_t s24[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION];
	int ns, nch;
	if (rx->cfg.accept == REAC_RX_ACCEPT_UPSTREAM) {
		nch = reac_upstream_channels(len);      /* < REAC_MAX_CHANNELS by contract */
		ns = nch > 0 ? reac_upstream_decode(frame, len, s24) : -1;
	} else {
		nch = mode->n_channels;
		/* Decode the standard 1492 B frame. A 1494 B frame is that same frame
		 * plus 2 bytes after C2 EA: decode the embedded REAC_FRAME_BYTES and
		 * ignore them. reac_frame_inspect requires exactly REAC_FRAME_BYTES, so
		 * never hand it the 1494 length. Those 2 bytes are the low 16 bits of
		 * the frame's own Ethernet FCS left behind by the capture path — not a
		 * protocol field, in either direction (#82, and libreac's <reac/reac.h>
		 * since 0.5.0). Never emit them. */
		ns = reac_decode(frame, REAC_FRAME_BYTES, mode, s24); /* out[(ch*ns+s)*3] */
	}
	if (ns < 0) {
		atomic_fetch_add_explicit(&rx->frames_bad, 1, memory_order_relaxed);
		return;
	}
	/* Bound ns/nch to the stack-buffer sizes before the conversion loop: a decoder
	 * returning ns > 12 or nch > 40 would overrun s24[]/planar[]. This is the
	 * producer thread, so the branch cost is irrelevant. */
	if (ns > REAC_SAMPLES_PER_PKT)
		ns = REAC_SAMPLES_PER_PKT;
	if (nch > REAC_MAX_CHANNELS)
		nch = REAC_MAX_CHANNELS;
	/* NOT zero-initialised, and the ring is why. This buffer used to be declared
	 * `= { 0 }` -- a 1920-byte memset every frame -- because the ring consumer
	 * reads all 40 rows and an 8-channel box fills eight, so the other 32 had to
	 * be handed over as real silence rather than stack junk. reac_ring_write now
	 * owns that guarantee: it is calloc'd silent and it zeroes any row a narrowing
	 * source abandons. So the rows past nch are never read from here and never
	 * need to exist. Rows 0..nch-1 are each written in full below.
	 *
	 * Do not reinstate the initialiser without also reinstating the 40-row write.
	 * Half of that pair is stale audio going to the graph. */
	float planar[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT];
	for (int ch = 0; ch < nch; ch++)
		for (int s = 0; s < ns; s++)
			planar[ch * ns + s] = reac_s24le_to_f32(&s24[(size_t)(ch * ns + s) * 3]);
	reac_ring_write(rx->ring, planar, (uint32_t)ns, (uint32_t)nch);
	atomic_fetch_add_explicit(&rx->frames_ok, 1, memory_order_relaxed);
}

void reac_rx_session_end(struct reac_rx *rx)
{
	if (!rx)
		return;
	atomic_fetch_add_explicit(&rx->peer_epoch, 1, memory_order_release);
}

void reac_rx_peer_reset(struct reac_rx *rx, const uint8_t mac[6], unsigned session)
{
	if (!rx || !mac)
		return;
	if (rx->up_src_locked && memcmp(rx->up_src, mac, 6) == 0 &&
	    rx->peer_session == session)
		return;                       /* same peer, same session — nothing was lost */
	rx->peer_session = session;
	memcpy(rx->up_src, mac, 6);
	rx->up_src_locked = 1;
	/* A new peer means a new session. Everything the loop carries about the old
	 * one is now a lie: its counter is not ours to continue, its rate slope
	 * describes a clock that left, and the duplicate guard's previous frame
	 * belongs to another box. The loop clears them when it sees the bump. */
	atomic_fetch_add_explicit(&rx->peer_epoch, 1, memory_order_release);
}

/* The stream gate: does this valid 0x8819 frame belong to the stream we
 * decode? DOWNSTREAM accepts only the fixed 1492 B broadcast. UPSTREAM
 * accepts box-shaped returns and locks onto the first box's src MAC so a
 * second box (or the master's own broadcast) can't interleave counters and
 * audio from two sources into one ring. */
static int gate_accepts(struct reac_rx *rx, const uint8_t *frame, size_t len)
{
	if (rx->cfg.accept == REAC_RX_ACCEPT_DOWNSTREAM)
		/* 1492 = the frame; 1494 = the same frame with 2 bytes of Ethernet FCS
		 * residue kept after the C2 EA end marker by the capture path (see the
		 * note in feed_frame and #82). Accept both — the mirror twin arrives as
		 * one of each and the dup guard below collapses the pair. */
		return len == (size_t)REAC_FRAME_BYTES ||
		       len == (size_t)REAC_FRAME_BYTES_OHRCA;
	if (reac_upstream_channels(len) < 0)
		return 0;
	if (!rx->up_src_locked) {
		memcpy(rx->up_src, frame + 6, 6);
		rx->up_src_locked = 1;
		return 1;
	}
	return memcmp(rx->up_src, frame + 6, 6) == 0;
}

/* Slow PI-style slope filter: nominal pps vs observed counter advance per
 * second. Updates ppm_error_milli a few times a second. The source node turns
 * that into io_rate_match.rate. Kept deliberately gentle — locked to the
 * long-term counter slope, not instantaneous packet jitter (the design's
 * Tier-A loop). */
static void update_ppm(struct reac_rx *rx, uint16_t counter, uint64_t now_ns)
{
	if (rx->ppm_have_last) {
		rx->ppm_win_frames += reac_counter_gap(rx->ppm_last_counter, counter) + 1;
	} else {
		rx->ppm_win_start_ns = now_ns;
		rx->ppm_have_last = 1;
	}
	rx->ppm_last_counter = counter;

	uint64_t dt = now_ns - rx->ppm_win_start_ns;
	if (dt >= 250000000ull) { /* recompute ~4x/s */
		double obs_pps = (double)rx->ppm_win_frames * 1e9 / (double)dt;
		double nom_pps = (double)rx->sample_rate / REAC_SAMPLES_PER_PKT;
		double ppm = (obs_pps - nom_pps) / nom_pps * 1e6;
		atomic_store_explicit(&rx->ppm_error_milli, (int)(ppm * 1000.0), memory_order_relaxed);
		/* Release, after the value: a consumer that keys off the seq must never see
		 * a bumped counter vouching for a stale estimate. */
		atomic_fetch_add_explicit(&rx->ppm_seq, 1, memory_order_release);
		rx->ppm_win_start_ns = now_ns;
		rx->ppm_win_frames = 0;
	}
}

static void *rx_loop(void *arg)
{
	struct reac_rx *rx = arg;
	const struct reac_mode *mode = reac_mode_for(rx->sample_rate);
	uint8_t frame[REAC_FRAME_BYTES + 64];

	struct reac_capture cap = { .fd = -1 };
	struct pcap_source ps = { 0 };
	int live = (rx->cfg.kind == REAC_RX_LIVE);

	/* Open the wire source inside the thread so the fd/FILE* lives and dies with
	 * it. (rate-detect in reac_rx_open used a SEPARATE short-lived capture.) */
	if (live) {
		if (reac_capture_open(&cap, rx->cfg.source) != 0) {
			/* reac_rx_open() already refused a missing/uncapable interface
			 * before this thread was ever spawned; reaching this a second
			 * time means the interface left BETWEEN that check and here. Loud
			 * either way — a feeder thread that returns NULL silently is
			 * exactly the "runs deaf, looks alive" failure this exists to
			 * rule out. */
			fprintf(stderr, "reac-pw: --live '%s': capture open failed at feeder "
			        "start (interface present a moment ago, gone now?) — RX is "
			        "NOT running\n", rx->cfg.source);
			return NULL;
		}
		reac_capture_set_nonblock(&cap, 0); /* blocking; EINTR/stop-flag exits */
		/* SO_RCVTIMEO so a traffic-idle recv() still wakes periodically to
		 * recheck rx->running. Needed because SIGINT/SIGTERM never reach this
		 * thread as an interrupting signal: main() registers them with
		 * pw_loop_add_signal() (signalfd-based) BEFORE reac_rx_start() spawns
		 * us, so pthread_create() inherits them already blocked in our mask —
		 * a blocked signal can't EINTR a blocking syscall, it just queues for
		 * the main thread's signalfd read. Without this timeout, an idle wire
		 * (box parked/PHY down/no more traffic) leaves recv() parked forever,
		 * reac_rx_stop()'s pthread_join() never returns, and SIGTERM is a
		 * silent no-op (only SIGKILL works). reac_slave.c's RX socket already
		 * carries the identical timeout for the same reason (see its
		 * SO_RCVTIMEO comment). */
		struct timeval tv = { 0, 200000 }; /* 200 ms: well under the shutdown budget */
		setsockopt(cap.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		/* Remember WHICH INTERFACE this socket is bound to, asked of the socket
		 * itself. Everything the alarm below decides is a comparison against
		 * this number. */
		atomic_store_explicit(&rx->bound_ifindex, capture_bound_ifindex(cap.fd),
		                      memory_order_release);
	} else {
		if (pcap_source_open(&ps, rx->cfg.source) != 0)
			return NULL;
	}

	uint16_t last_counter = 0;
	int have_counter = 0;
	unsigned seen_epoch = atomic_load_explicit(&rx->peer_epoch, memory_order_acquire);
	uint64_t pcap_first_ts = 0, wall_first_ns = 0;
	uint64_t last_stat_ns = 0;   /* periodic RX telemetry (every ~2 s) */
	uint64_t last_iface_check_ns = 0;   /* the vanished-interface alarm below */

	while (atomic_load_explicit(&rx->running, memory_order_acquire)) {
		long n;
		uint64_t pcap_ts = 0;
		if (live) {
			/* THE LOUD ALTERNATIVE TO SILENT PROBING. recv() on a socket whose
			 * interface went away mid-run does not reliably error — it can just
			 * keep timing out on SO_RCVTIMEO exactly like a genuinely idle wire,
			 * so the master pacer above logs "still PROBING" forever with
			 * nothing to tell the two apart. Ask directly, every ~2 s.
			 *
			 * Ask about the BINDING, not the name. This check used to be
			 * `!reac_rx_iface_present(source)` — "does the name still resolve to
			 * anything?" — and that is a false negative in the exact case that
			 * matters most. 2026-08-29, the S-0808 segment: the AX88179 was
			 * unplugged at 22:14:20 and came back at 22:17:00 under the SAME
			 * name and SAME MAC with a NEW ifindex. The alarm fired while the
			 * name was absent, then FELL SILENT when it returned, while this
			 * socket stayed bound to the dead index — deaf, and mute on TX. The
			 * operator was left with "still PROBING ... bounce the box PHY" and
			 * spent the outage replugging a box that was never at fault.
			 *
			 * Comparing indices covers the rename and the removal too: both make
			 * the name resolve to something that is not what we bound. */
			uint64_t chk_now = mono_ns();
			if (chk_now - last_iface_check_ns >= 2000000000ull) {
				last_iface_check_ns = chk_now;
				unsigned bound = atomic_load_explicit(&rx->bound_ifindex,
				                                      memory_order_acquire);
				unsigned now_idx = reac_rx_iface_index(rx->cfg.source);
				if (reac_rx_binding_lost(bound, now_idx)) {
					/* Say it ONCE and stop: this is fatal, not a condition to
					 * narrate every 2 s. The old alarm repeated forever because
					 * it had no way to end the process; now it does. */
					if (now_idx == 0)
						fprintf(stderr, "reac-pw: LIVE INTERFACE '%s' (ifindex %u) IS "
						        "GONE — the name no longer resolves to any interface "
						        "(removed, or renamed). ", rx->cfg.source, bound);
					else
						fprintf(stderr, "reac-pw: LIVE INTERFACE '%s' WAS REPLACED — "
						        "the name now resolves to ifindex %u, but this socket "
						        "is bound to %u (a USB NIC re-enumerated: same name, "
						        "different interface). ", rx->cfg.source, now_idx, bound);
					fprintf(stderr, "The capture socket is deaf and its TX is mute: no "
					        "packet can arrive on it or leave it again, and NO BOX CAN "
					        "ANSWER A MASTER THAT IS NOT TRANSMITTING. This is NOT a "
					        "dead box — do not bounce the box. Exiting so the service "
					        "manager restarts us onto the live interface.\n");
					atomic_store_explicit(&rx->iface_lost, 1, memory_order_release);
					break;   /* the main loop polls the latch and terminates */
				}
			}
			n = reac_capture_next(&cap, frame, sizeof frame);
		} else {
			n = pcap_source_next(&ps, frame, sizeof frame, &pcap_ts);
			if (n == 0) { /* EOF: loop the fixture for a steady offline source.
			               * Reset the pacing baseline (wall_first_ns) so the new
			               * loop re-paces from its first timestamp — otherwise
			               * every loop after the first replays FLAT OUT (targets
			               * land in the past), flooding the ring. */
				pcap_source_close(&ps); pcap_source_open(&ps, rx->cfg.source);
				have_counter = 0; wall_first_ns = 0;
				rx->have_prev_frame = 0; /* no cross-loop-seam duplicate match */
				rx->ppm_have_last = 0; rx->ppm_win_frames = 0; /* drop the stale ppm
				               * window so the next loop doesn't spike one bogus ppm
				               * off a counter discontinuity across the seam */
				continue;
			}
		}
		if (n <= 0)
			continue;
		if (!reac_frame_is_reac(frame, (size_t)n))
			continue;
		if (!gate_accepts(rx, frame, (size_t)n)) {
			/* the other direction's stream (or another box): not ours */
			atomic_fetch_add_explicit(&rx->frames_other, 1, memory_order_relaxed);
			continue;
		}

		/* Duplicate-frame guard (see reac_rx.h): drop a frame whose CLEAN prefix
		 * is byte-identical to the one before it. Comparing on the clean length
		 * rather than the wire length is what makes it catch the mirror twin,
		 * whose two copies differ only in that one of them kept 2 bytes of FCS
		 * (1492 vs 1494) — a raw length compare never fires on that pair. The
		 * over-clock repeat (equal lengths, so equal clean lengths) is subsumed.
		 * Feeding both copies doubles every 12-sample block into a granular
		 * stutter and doubles the effective rate. Runs before the counter/ppm/
		 * decode so the rate estimator and the ring see the real cadence. */
		size_t clean = reac_frame_clean_len((size_t)n);
		if (rx->have_prev_frame && clean == rx->prev_clean_len &&
		    memcmp(frame, rx->prev_frame, clean) == 0) {
			atomic_fetch_add_explicit(&rx->frames_dup, 1, memory_order_relaxed);
			continue;
		}
		if (clean <= sizeof rx->prev_frame) {
			memcpy(rx->prev_frame, frame, clean);
			rx->prev_clean_len = clean;
			rx->have_prev_frame = 1;
		}

		/* optional: pace pcap replay by capture timestamps so the rate loop
		 * sees realistic cadence offline */
		if (!live && rx->cfg.pcap_realtime && pcap_ts) {
			if (!wall_first_ns) { wall_first_ns = mono_ns(); pcap_first_ts = pcap_ts; }
			uint64_t target = wall_first_ns + (pcap_ts - pcap_first_ts) * 1000ull;
			struct timespec ts = { target / 1000000000ull, target % 1000000000ull };
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
		}

		uint16_t counter = reac_frame_counter(frame);
		/* THE PEER CHANGED: drop everything learned from the last one, in one
		 * place, before it can be mistaken for continuity. Counting the seam as
		 * lost frames is how a clean reconnect reported thousands of gaps. */
		unsigned ep = atomic_load_explicit(&rx->peer_epoch, memory_order_acquire);
		if (ep != seen_epoch) {
			seen_epoch = ep;
			have_counter = 0;
			rx->have_prev_frame = 0;
			rx->ppm_have_last = 0;
			rx->ppm_win_frames = 0;
			atomic_store_explicit(&rx->ppm_error_milli, 0, memory_order_relaxed);
			reac_pace_watch_reset(&rx->pace, mono_ns());
		}
		if (have_counter) {
			uint16_t gap = reac_counter_gap(last_counter, counter);
			if (gap)
				atomic_fetch_add_explicit(&rx->counter_gaps, gap, memory_order_relaxed);
		}
		last_counter = counter;
		have_counter = 1;

		uint64_t now = mono_ns();
		update_ppm(rx, counter, now);
		feed_frame(rx, mode, frame, (size_t)n);

		/* THE PACE ALARM. Not gated on REAC_DEBUG: a stream labelled at twice the
		 * rate it carries raises no xrun and no error — PipeWire believes the label
		 * — so this is the only thing that will ever say it. Measured on the rig
		 * 2026-08-22 with --rate 96000: a byte-perfect 7996 pps downstream and both
		 * boxes answering 4000 pps, published as 96000 Hz over 48 000 samples/s
		 * with gaps=0. Rate-limited inside the watcher. */
		if (reac_pace_watch_frame(&rx->pace, now)) {
			int obs = reac_pace_watch_observed(&rx->pace);
			fprintf(stderr,
			        "reac-pw: WIRE PACE MISMATCH — configured %d Hz (%d pps) but the "
			        "segment is carrying %d Hz (%d pps) from "
			        "%02x:%02x:%02x:%02x:%02x:%02x. The published node rate is a LIE "
			        "at this point: audio is being labelled %.2gx its true rate. "
			        "Set --rate %d, or find why the box did not follow.\n",
			        rx->sample_rate, rx->sample_rate / REAC_SAMPLES_PER_PKT,
			        obs, obs / REAC_SAMPLES_PER_PKT,
			        rx->up_src[0], rx->up_src[1], rx->up_src[2],
			        rx->up_src[3], rx->up_src[4], rx->up_src[5],
			        (double)rx->sample_rate / (double)obs, obs);
		}

		/* Opt-in RX telemetry (REAC_DEBUG) — the decode is invisible otherwise;
		 * this is how you tell "gate rejecting" (frames_other climbs) from
		 * "decoded fine, audio lost downstream" (frames_ok climbs). ~every 2 s. */
		static int dbg = -1;
		if (dbg < 0)
			dbg = getenv("REAC_DEBUG") != NULL;
		if (dbg && now - last_stat_ns >= 2000000000ull) {
			last_stat_ns = now;
			/* NAMED BY ITS SOURCE, because one daemon runs several feeders. The
			 * counters used to be printed unattributed, so on a host serving more
			 * than one segment there was no way to tell whose stream a line was
			 * about — and a peer that broadcasts (a box mastering the wire) never
			 * locks `src`, so the address on the line cannot stand in for it. */
			fprintf(stderr, "reac_rx: [%s] ok=%llu dup=%llu other=%llu bad=%llu gaps=%llu"
			        " src=%02x:%02x:%02x:%02x:%02x:%02x%s | out: active_ch=%d"
			        " peak=%.6f fill=%d\n",
			        rx->cfg.source ? rx->cfg.source : "?",
			        (unsigned long long)atomic_load(&rx->frames_ok),
			        (unsigned long long)atomic_load(&rx->frames_dup),
			        (unsigned long long)atomic_load(&rx->frames_other),
			        (unsigned long long)atomic_load(&rx->frames_bad),
			        (unsigned long long)atomic_load(&rx->counter_gaps),
			        rx->up_src[0], rx->up_src[1], rx->up_src[2],
			        rx->up_src[3], rx->up_src[4], rx->up_src[5],
			        rx->up_src_locked ? "" : " (unlocked)",
			        atomic_load(&rx->src_active_ch),
			        atomic_load(&rx->src_peak_micro) / 1e6,
			        atomic_load(&rx->src_fill));
		}
	}

	if (live)
		reac_capture_close(&cap);
	else
		pcap_source_close(&ps);
	return NULL;
}

int reac_rx_open(struct reac_rx *rx, const struct reac_rx_cfg *cfg, struct reac_ring *ring)
{
	memset(rx, 0, sizeof *rx);
	rx->cfg = *cfg;
	rx->ring = ring;

	if (cfg->kind == REAC_RX_LIVE) {
		/* Open (and validate) the interface UNCONDITIONALLY, even when --rate
		 * forces the sample rate and no detection is needed below: this is the
		 * only place a dead, renamed, or never-existed --live NIC can be
		 * refused before reac_rx_start() spawns the feeder thread. A capture
		 * failure discovered only inside that thread (rx_loop) has nowhere to
		 * report to but its own silent `return NULL` — the daemon keeps
		 * running with no RX, no error, and no exit, indistinguishable from a
		 * box that is genuinely dead. Fail loudly HERE, and name the
		 * interface. */
		struct reac_capture cap = { .fd = -1 };
		if (reac_capture_open(&cap, cfg->source) != 0) {
			fprintf(stderr, "reac-pw: --live '%s': no such interface, or "
			        "insufficient capability (needs CAP_NET_RAW) — refusing to "
			        "start rather than run a capture socket that can never "
			        "receive\n", cfg->source);
			return -1;
		}
		if (cfg->forced_rate) {
			rx->sample_rate = cfg->forced_rate;
		} else {
			int r = reac_detect_rate_fd(cap.fd, 500);
			rx->sample_rate = r > 0 ? r : 48000; /* default when no traffic yet */
		}
		reac_capture_close(&cap);
	} else if (cfg->forced_rate) {
		rx->sample_rate = cfg->forced_rate;
	} else {
		/* offline: peek a few frames to snap the rate from inter-arrival cadence
		 * would need timestamps; for pcap we accept forced_rate or default 48k. */
		rx->sample_rate = 48000;
	}

	/* `--rate` arrives as cfg->forced_rate and SKIPS the detection above, so nothing
	 * downstream ever compared the configured pace against the wire. The watcher is the
	 * comparison, and it runs whichever way sample_rate was arrived at: a detected rate
	 * can also go stale when a box is swapped for one that paces differently. */
	reac_pace_watch_init(&rx->pace, rx->sample_rate);

	/* ~250 ms of ring at the recovered rate, power-of-two rounded inside init */
	uint32_t depth = (uint32_t)(rx->sample_rate / 4);
	if (reac_ring_init(ring, REAC_MAX_CHANNELS, depth) != 0)
		return -1;
	return 0;
}

int reac_rx_start(struct reac_rx *rx)
{
	/* The feeder thread owns the wire source (opened in rx_loop). reac_rx_open
	 * already detected the rate + sized the ring; here we just flip running and
	 * spawn. This is the PRODUCER thread — plain SCHED_OTHER. Only the audio
	 * graph (and, in Phase 2, the TX slot pacer) run SCHED_FIFO. */
	atomic_store_explicit(&rx->running, 1, memory_order_release);
	return pthread_create(&rx->thread, NULL, rx_loop, rx);
}

void reac_rx_stop(struct reac_rx *rx)
{
	atomic_store_explicit(&rx->running, 0, memory_order_release);
	pthread_join(rx->thread, NULL);
}

void reac_rx_close(struct reac_rx *rx)
{
	(void)rx; /* ring is freed by the owner */
}
