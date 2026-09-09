// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* fake-box-master — a stagebox with its REAC Mode switch on M, on a real wire.
 *
 * WHY THIS EXISTS AND WHY IT IS NOT reac-pw. The daemon has no mode that emits a
 * box-width MASTER: the master role broadcasts the fixed 1492 B, 40-channel downstream
 * (that is what a desk is), and the slave role unicasts a box-width return to a master it
 * learned. A stagebox on M does neither — it broadcasts its OWN upstream geometry,
 * `52 + n*36`, and runs no handshake at all (reac-protocol/wire-format.md, "The stagebox's
 * REAC Mode switch"). Since 0.5.1 that peer is the one an unpinned wire JOINS and a wire
 * pinned master REFUSES, so the veth proof needs one on the far end of the cable, and
 * putting a fake-master mode into the shipped binary to get it would be a mode nothing
 * else ever uses.
 *
 * THE BYTES ARE THE UNIT FIXTURE'S BYTES. Frames come from the same libreac builders
 * test_reac_hunt.c's `box_on_m` uses — a broadcast flood FILLER at the box's width, and
 * once a second the same frame with a MASTER-ONLY head-amp record stamped over its control
 * block and the block checksum re-applied. That second frame is what makes the peer a
 * MASTER to the classifier (only a console emits preamp records) while its LENGTH keeps
 * saying box. Nothing here hand-rolls a frame, so a change to the wire format cannot leave
 * this test asserting bytes the daemon no longer speaks.
 *
 * It transmits only, on the interface it is given, and stops on SIGTERM.
 *
 *   usage: fake-box-master <iface> <aa:bb:cc:dd:ee:ff> <channels> [fps]
 */
#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_now;
static void on_term(int sig) { (void)sig; stop_now = 1; }

static int parse_mac(const char *s, uint8_t out[6])
{
	unsigned v[6];
	if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
		return -1;
	for (int i = 0; i < 6; i++)
		out[i] = (uint8_t)v[i];
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s <iface> <src-mac> <channels> [fps]\n", argv[0]);
		return 2;
	}
	const char *iface = argv[1];
	uint8_t src[6];
	if (parse_mac(argv[2], src) != 0) {
		fprintf(stderr, "fake-box-master: '%s' is not a MAC\n", argv[2]);
		return 2;
	}
	int n_ch = atoi(argv[3]);
	int fps = argc > 4 ? atoi(argv[4]) : 2000;
	if (n_ch <= 0 || n_ch >= REAC_MAX_CHANNELS || fps <= 0) {
		fprintf(stderr, "fake-box-master: a box width is 1..%d channels and fps > 0\n",
		        REAC_MAX_CHANNELS - 1);
		return 2;
	}

	int fd = socket(AF_PACKET, SOCK_RAW, htons(0x8819));
	if (fd < 0) {
		fprintf(stderr, "fake-box-master: AF_PACKET: %s (need CAP_NET_RAW)\n",
		        strerror(errno));
		return 1;
	}
	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", iface);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) != 0) {
		fprintf(stderr, "fake-box-master: no interface '%s': %s\n", iface, strerror(errno));
		return 1;
	}
	struct sockaddr_ll sll;
	memset(&sll, 0, sizeof sll);
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(0x8819);
	sll.sll_ifindex = ifr.ifr_ifindex;
	sll.sll_halen = 6;
	memset(sll.sll_addr, 0xff, 6);

	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);

	static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	/* THE BOX'S MICROPHONES, one distinct constant per channel — the same pattern
	 * tests/test_reac_box_master_audio.c reads back by value out of the ring. A flood
	 * filler with NULL audio is silence, and a segment that decoded every frame into
	 * silence reads exactly like one that decoded nothing: this is what makes the
	 * frames the daemon counts frames that CARRY something. */
	float pcm[REAC_MAX_CHANNELS][REAC_SAMPLES_PER_PKT];
	float *planar[REAC_MAX_CHANNELS];
	for (int c = 0; c < REAC_MAX_CHANNELS; c++) {
		planar[c] = pcm[c];
		for (int s = 0; s < REAC_SAMPLES_PER_PKT; s++)
			pcm[c][s] = (float)(c + 1) / 64.0f;
	}
	uint8_t f[2048];
	uint16_t counter = 0;
	long sent = 0, announces = 0;
	struct timespec period = { 0, 0 };
	period.tv_nsec = 1000000000L / fps;

	fprintf(stderr, "fake-box-master: %s, %d ch (%zu B frames) at ~%d fps from "
	        "%02x:%02x:%02x:%02x:%02x:%02x — broadcast box geometry carrying a distinct "
	        "constant per channel, one master-only record per second, no handshake of "
	        "any kind\n",
	        iface, n_ch, reac_ctrl_box_frame_len(n_ch), fps,
	        src[0], src[1], src[2], src[3], src[4], src[5]);

	while (!stop_now) {
		size_t n = reac_ctrl_build_flood_filler(f, BCAST, src, counter++, n_ch,
		                                        planar, REAC_SAMPLES_PER_PKT);
		if (n == 0)
			break;
		/* ONE FRAME A SECOND CARRIES THE MASTER SIGNATURE. A head-amp record is
		 * console-only (reac_disco.c's role_of), so this is what files the peer as a
		 * MASTER — at a length that is unambiguously a box's. The block checksum is
		 * re-applied after the stamp, or the sighting is discarded as corrupt. */
		if (sent % fps == 0) {
			if (reac_ctrl_stamp_headamp(f, 0x20, 0 /* phantom */, 1) != 0)
				break;
			reac_ctrl_checksum_apply(f);
			announces++;
		}
		/* ENETDOWN IS NOT AN ERROR HERE, IT IS "NOT YET". The proof starts this box
		 * BEFORE it raises the link, so that the wire carries a box master from the
		 * first instant of carrier and the daemon's masterless observation cannot win
		 * a race it was never meant to be in. A frame sent into a down interface is
		 * dropped by the kernel; the loop simply keeps offering. */
		if (sendto(fd, f, n, 0, (struct sockaddr *)&sll, sizeof sll) < 0 &&
		    errno != ENOBUFS && errno != EAGAIN && errno != ENETDOWN) {
			fprintf(stderr, "fake-box-master: send: %s\n", strerror(errno));
			break;
		}
		sent++;
		nanosleep(&period, NULL);
	}
	fprintf(stderr, "fake-box-master: stopped after %ld frames (%ld master records)\n",
	        sent, announces);
	close(fd);
	return 0;
}
