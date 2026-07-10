// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reac_slave.h"
#include "reac_ctrl.h"

#include <reac/reac.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <arpa/inet.h>     /* htons */

/* ------------------------------------------------------------------------- *
 * The PURE decision core — maps the reac_fsm action onto a slave emit kind.
 *
 * reac_fsm is the brain (the gold §13d slave establishment + HOLD). reac_slave
 * adds nothing to the state logic; it only translates the FSM's action into the
 * concrete frame the slave TX path must put on the wire, and threads the FSM's
 * emit_heartbeat flag through. Keeping this a thin, pure mapping means the FSM
 * stays the single source of truth + this layer is trivially offline-testable.
 * ------------------------------------------------------------------------- */

static struct reac_slave_decision map_action(const struct reac_fsm_out *o)
{
	struct reac_slave_decision d = { REAC_SLAVE_EMIT_NONE, 0, o->state };
	switch (o->action) {
	case FSM_ACT_FLOOD_BCAST:
		d.emit = REAC_SLAVE_EMIT_FLOOD_FILLER;
		break;
	case FSM_ACT_EMIT_JOIN:
		d.emit = REAC_SLAVE_EMIT_JOIN;
		break;
	case FSM_ACT_UNICAST_AUDIO:
		d.emit = REAC_SLAVE_EMIT_UPSTREAM_AUDIO;
		d.with_heartbeat = o->emit_heartbeat;   /* FSM gates the ~1/s keep-alive */
		break;
	case FSM_ACT_SILENCE:   /* TX-mute window: counter free-runs, nothing emitted */
	case FSM_ACT_STOP:
	case FSM_ACT_NONE:
	default:
		d.emit = REAC_SLAVE_EMIT_NONE;
		break;
	}
	return d;
}

void reac_slave_fsm_init(struct reac_slave *s, const struct reac_slave_cfg *cfg)
{
	memset(s, 0, sizeof *s);
	s->fd = -1;
	reac_fsm_init(&s->fsm);
	s->box_channels = (cfg && cfg->box_channels > 0)
		? (cfg->box_channels > REAC_MAX_CHANNELS ? REAC_MAX_CHANNELS : cfg->box_channels)
		: REAC_SLAVE_BOX_CHANNELS_DEFAULT;
	s->sample_rate = cfg ? cfg->sample_rate : 0;

	static const uint8_t standin[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x41 };
	memcpy(s->src, (cfg && cfg->src_mac) ? cfg->src_mac : standin, 6);
}

struct reac_slave_decision reac_slave_step_rx(struct reac_slave *s,
                                              const struct reac_ctrl_parsed *rx)
{
	struct reac_fsm_out o = reac_fsm_step(&s->fsm, FSM_EV_RX, rx);
	atomic_store_explicit(&s->established, s->fsm.state == FSM_ESTABLISHED ? 1 : 0,
	                      memory_order_relaxed);
	return map_action(&o);
}

struct reac_slave_decision reac_slave_step_tick(struct reac_slave *s)
{
	struct reac_fsm_out o = reac_fsm_step(&s->fsm, FSM_EV_TICK, NULL);
	atomic_store_explicit(&s->established, s->fsm.state == FSM_ESTABLISHED ? 1 : 0,
	                      memory_order_relaxed);
	return map_action(&o);
}

struct reac_slave_decision reac_slave_step_phy(struct reac_slave *s, int up)
{
	struct reac_fsm_out o = reac_fsm_step(&s->fsm,
	                                      up ? FSM_EV_PHY_UP : FSM_EV_PHY_DOWN, NULL);
	atomic_store_explicit(&s->established, s->fsm.state == FSM_ESTABLISHED ? 1 : 0,
	                      memory_order_relaxed);
	return map_action(&o);
}

/* ------------------------------------------------------------------------- *
 * The live I/O engine — the shell around the FSM.
 *
 * One AF_PACKET socket, bound to the REAC NIC, does BOTH directions: RX the
 * master's downstream + TX our upstream. The master frame is the clock: each RX
 * of a master frame is one tick, and we emit exactly one upstream frame in
 * response (frame-arrival = our slot clock — the master owns the rate). We do NOT
 * run a clock_nanosleep pacer; locking to the master cadence is the whole point.
 * A short non-blocking idle poll only services the FLOOD re-emit + the TX-mute
 * dwell while no master frame is arriving yet.
 * ------------------------------------------------------------------------- */

/* Pull our box_channels of input from the tx_ring into a planar view; on underrun
 * (or no ring) the channels are silent. Returns the per-channel sample count
 * staged (REAC_SAMPLES_PER_PKT or 0). */
static int stage_inputs(struct reac_slave *s,
                        float buf[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT],
                        float *planar[REAC_MAX_CHANNELS])
{
	for (int c = 0; c < s->box_channels; c++) {
		planar[c] = buf[c];
		for (int sm = 0; sm < REAC_SAMPLES_PER_PKT; sm++)
			buf[c][sm] = 0.0f;
	}
	if (!s->tx_ring)
		return 0;
	uint32_t got = reac_ring_read_planar(s->tx_ring, planar, (uint32_t)s->box_channels,
	                                      REAC_SAMPLES_PER_PKT);
	return got > 0 ? REAC_SAMPLES_PER_PKT : 0;
}

/* Emit one frame for the decision `d` on the wire. `bcast` = the broadcast dst
 * (presence-flood), else the learned master MAC (unicast linked traffic). */
static void emit_decision(struct reac_slave *s, const struct reac_slave_decision *d,
                          struct sockaddr_ll *bcast_sll, struct sockaddr_ll *uni_sll)
{
	static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	uint8_t frame[2048];
	size_t len = 0;
	struct sockaddr_ll *sll = uni_sll;
	uint16_t counter = s->fsm.counter;

	float buf[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
	float *planar[REAC_MAX_CHANNELS];

	switch (d->emit) {
	case REAC_SLAVE_EMIT_NONE:
		return;

	case REAC_SLAVE_EMIT_FLOOD_FILLER:
		/* §13d step 1: announce by FLOODING broadcast FILLER while unlinked. The
		 * dst is broadcast; master is not learned yet. Box-width FILLER, silent. */
		stage_inputs(s, buf, planar);  /* may carry early input; harmless pre-link */
		len = reac_ctrl_build_upstream_filler(frame, BCAST, s->src, counter,
		                                      s->box_channels, planar,
		                                      REAC_SAMPLES_PER_PKT);
		sll = bcast_sll;
		break;

	case REAC_SLAVE_EMIT_JOIN:
		/* §13b/§13d: the cold-connect (cdea 04 03, sub-cmd 04 — the trigger) plus a
		 * config-announce. These are the reconstructed JOIN builders (experimental,
		 * gated until a fresh PHY-link-up rig capture confirms the grant-burst), so
		 * a real link only completes when the master's cdea 04 03 grant is RX'd. The
		 * cold-connect is unicast-to-master once learned, else broadcast-flooded. */
		if (s->fsm.have_master) {
			len = reac_ctrl_build_coldconnect(frame, s->fsm.master_mac, s->src, counter);
		} else {
			len = reac_ctrl_build_coldconnect(frame, BCAST, s->src, counter);
			sll = bcast_sll;
		}
		break;

	case REAC_SLAVE_EMIT_UPSTREAM_AUDIO:
		/* Established: unicast our input channels upstream at the box's slots — a
		 * slave sends its inputs INTO the REAC stream. The master MAC is learned. */
		stage_inputs(s, buf, planar);
		len = reac_ctrl_build_upstream_filler(frame, s->fsm.master_mac, s->src, counter,
		                                      s->box_channels, planar,
		                                      REAC_SAMPLES_PER_PKT);
		break;

	case REAC_SLAVE_EMIT_HEARTBEAT:
		len = reac_ctrl_build_box_hb(frame, s->fsm.master_mac, s->src, counter);
		break;
	}

	if (len == 0)
		return;

	ssize_t r = sendto(s->fd, frame, len, 0, (struct sockaddr *)sll, sizeof *sll);
	if (r < 0)
		atomic_fetch_add_explicit(&s->tx_errors, 1, memory_order_relaxed);
	else
		atomic_fetch_add_explicit(&s->tx_frames, 1, memory_order_relaxed);

	/* The established keep-alive rides alongside the upstream audio frame on the
	 * tick the FSM flags it (it replaces a slot, like a box's sparse heartbeat). */
	if (d->emit == REAC_SLAVE_EMIT_UPSTREAM_AUDIO && d->with_heartbeat) {
		uint8_t hb[2048];
		size_t hn = reac_ctrl_build_box_hb(hb, s->fsm.master_mac, s->src,
		                                   (uint16_t)(counter + 1));
		ssize_t hr = sendto(s->fd, hb, hn, 0, (struct sockaddr *)uni_sll, sizeof *uni_sll);
		if (hr < 0)
			atomic_fetch_add_explicit(&s->tx_errors, 1, memory_order_relaxed);
		else
			atomic_fetch_add_explicit(&s->tx_frames, 1, memory_order_relaxed);
	}
}

static void *slave_loop(void *arg)
{
	struct reac_slave *s = arg;

	struct sockaddr_ll bcast_sll, uni_sll;
	memset(&bcast_sll, 0, sizeof bcast_sll);
	bcast_sll.sll_family = AF_PACKET;
	bcast_sll.sll_ifindex = s->ifindex;
	bcast_sll.sll_halen = 6;
	memset(bcast_sll.sll_addr, 0xFF, 6);
	uni_sll = bcast_sll;   /* sll_addr patched to the master MAC once learned */

	uint8_t rxbuf[2048];

	while (atomic_load_explicit(&s->running, memory_order_acquire)) {
		/* Apply a pending PHY change on THIS thread (the FSM owner). */
		int want = atomic_load_explicit(&s->phy_up_req, memory_order_acquire);
		if (want != s->phy_up_seen) {
			struct reac_slave_decision d = reac_slave_step_phy(s, want);
			s->phy_up_seen = want;
			if (want)               /* PHY up: begin the presence-flood immediately */
				emit_decision(s, &d, &bcast_sll, &uni_sll);
		}

		/* Block (with a short timeout) for the next master frame. Frame-arrival is
		 * the clock tick we lock to; a timeout services the flood/dwell self-clock. */
		ssize_t n = recv(s->fd, rxbuf, sizeof rxbuf, 0);
		if (n <= 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				/* No master frame this window — self-clock the FSM one tick so the
				 * FLOOD re-emit + TX-mute dwell + peer-gone countdown still advance. */
				struct reac_slave_decision d = reac_slave_step_tick(s);
				memcpy(uni_sll.sll_addr, s->fsm.master_mac, 6);
				emit_decision(s, &d, &bcast_sll, &uni_sll);
				continue;
			}
			if (errno == EINTR)
				continue;
			break;
		}
		if (!reac_frame_is_reac(rxbuf, (size_t)n))
			continue;

		struct reac_ctrl_parsed p;
		reac_ctrl_parse(rxbuf, (size_t)n, &p);

		/* Only frames FROM a master are the clock + drive establishment; ignore any
		 * frame whose source is us (loopback) or another box. The FSM learns the
		 * master MAC from the L2 source of a master-kind frame. */
		if (memcmp(p.src, s->src, 6) == 0)
			continue;   /* our own echo on a hub/loopback — not a tick */

		atomic_fetch_add_explicit(&s->rx_master_frames, 1, memory_order_relaxed);

		struct reac_slave_decision d = reac_slave_step_rx(s, &p);
		memcpy(uni_sll.sll_addr, s->fsm.master_mac, 6);  /* learned this step */
		emit_decision(s, &d, &bcast_sll, &uni_sll);
	}
	return NULL;
}

/* ---- lifecycle ---------------------------------------------------------- */

int reac_slave_open(struct reac_slave *s, const struct reac_slave_cfg *cfg,
                    struct reac_ring *tx_ring)
{
	reac_slave_fsm_init(s, cfg);
	s->tx_ring = tx_ring;

	int fd = socket(AF_PACKET, SOCK_RAW, htons(REAC_ETHERTYPE));
	if (fd < 0)
		return -1;

	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, cfg->ifname, IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
		close(fd);
		return -1;
	}
	s->ifindex = ifr.ifr_ifindex;

	/* Bind to the interface so recv() yields only this NIC's 0x8819 frames. */
	struct sockaddr_ll sll;
	memset(&sll, 0, sizeof sll);
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(REAC_ETHERTYPE);
	sll.sll_ifindex = s->ifindex;
	if (bind(fd, (struct sockaddr *)&sll, sizeof sll) < 0) {
		close(fd);
		return -1;
	}

	/* Short RX timeout so recv() returns periodically to self-clock the flood/dwell
	 * even before any master frame arrives (~5 ms — well under the 600-frame HOLD
	 * budget at every rate, and the master frame, when present, clocks us instead). */
	struct timeval tv = { 0, 5000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

	s->fd = fd;
	return 0;
}

int reac_slave_start(struct reac_slave *s)
{
	atomic_store_explicit(&s->running, 1, memory_order_release);
	if (pthread_create(&s->thread, NULL, slave_loop, s) != 0) {
		atomic_store_explicit(&s->running, 0, memory_order_release);
		return -1;
	}
	return 0;
}

void reac_slave_set_phy_up(struct reac_slave *s, int up)
{
	atomic_store_explicit(&s->phy_up_req, up ? 1 : 0, memory_order_release);
}

void reac_slave_stop(struct reac_slave *s)
{
	if (!atomic_load_explicit(&s->running, memory_order_acquire))
		return;
	atomic_store_explicit(&s->running, 0, memory_order_release);
	pthread_join(s->thread, NULL);
}

void reac_slave_close(struct reac_slave *s)
{
	if (s->fd >= 0)
		close(s->fd);
	s->fd = -1;
}
