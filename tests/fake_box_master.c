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
 * IT ALSO LISTENS, WHEN ASKED (0.5.5). A box on M used to be a peer nothing was ever
 * sent to, so an emulator that only transmitted was the whole of it. Since the operator's
 * ruling — sending is always the same, being clock slave is only part of the enrolment —
 * the daemon DRIVES this wire, and the only place that claim can be measured is the far
 * end. With a report file the emulator opens a second socket, counts what arrives, decodes
 * it through libreac's own oracles (`reac_decode` for the 40-slot downstream audio,
 * `reac_ctrl_parse` for the control block) and writes a snapshot every ~300 ms, replaced
 * atomically so a reader never sees half of one. NOTHING here hand-rolls a decoder: the
 * emulator must not be able to agree with a daemon that both got the frame layout wrong.
 *
 * SIGUSR1 pauses and resumes TRANSMISSION while the listening side keeps counting, which
 * is how "a timeout is not a slot" is measured: the box goes quiet and the downstream must
 * stop with it.
 *
 * It stops on SIGTERM.
 *
 *   usage: fake-box-master <iface> <aa:bb:cc:dd:ee:ff> <channels> [fps] [report-file]
 */
#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_decode.h>
#include <reac/reac_sample.h>

#include <math.h>

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
static volatile sig_atomic_t tx_paused;
static void on_usr1(int sig) { (void)sig; tx_paused = !tx_paused; }

/* ---- the listening half ---------------------------------------------------
 * Counters are cumulative (a reader takes two samples and divides, which is what
 * makes a RATIO rather than a total); the per-channel energy is windowed and reset
 * at every write, so the file always describes the last ~300 ms and a tone that
 * started a second ago is not diluted by the silence before it. */
struct ear {
	int fd;
	unsigned long rx_down;          /* 1492/1494 B frames from somebody else */
	unsigned long rx_other;         /* 0x8819, but not the downstream we expect */
	unsigned long rx_before_tx;     /* downstream frames seen before our FIRST frame */
	unsigned long rx_bad_decode;
	size_t last_len;
	/* windowed per-channel energy, over the 40-slot downstream fabric */
	double sumsq[REAC_MAX_CHANNELS];
	double peak[REAC_MAX_CHANNELS];
	unsigned long nsamp;
	/* the last head-amp record decoded out of a control block, and how many */
	int ha_seen;
	unsigned ha_ch, ha_param, ha_value;
	unsigned long ha_count;
};

static void ear_ingest(struct ear *e, const uint8_t *f, size_t n, const uint8_t src[6],
                       unsigned long tx_so_far, const struct reac_mode *mode)
{
	if (n < 14 || f[12] != 0x88 || f[13] != 0x19)
		return;
	if (memcmp(f + 6, src, 6) == 0)
		return;                    /* our own egress, if the kernel ever echoes it */
	if (n != (size_t)REAC_FRAME_BYTES && n != (size_t)REAC_FRAME_BYTES_OHRCA) {
		e->rx_other++;
		return;
	}
	e->rx_down++;
	e->last_len = n;
	if (tx_so_far == 0)
		e->rx_before_tx++;

	/* THE AUDIO, through the same decoder the daemon's own capture path uses. */
	static uint8_t s24[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION];
	int ns = reac_decode(f, REAC_FRAME_BYTES, mode, s24);
	if (ns < 0) {
		e->rx_bad_decode++;
	} else {
		for (int c = 0; c < mode->n_channels && c < REAC_MAX_CHANNELS; c++)
			for (int i = 0; i < ns; i++) {
				float v = reac_s24le_to_f32(&s24[(size_t)(c * ns + i) * 3]);
				e->sumsq[c] += (double)v * (double)v;
				double a = v < 0 ? -(double)v : (double)v;
				if (a > e->peak[c])
					e->peak[c] = a;
			}
		e->nsamp += (unsigned long)ns;
	}

	/* THE CONTROL BLOCK, through libreac's own classifier. */
	struct reac_ctrl_parsed pr;
	if (reac_ctrl_parse(f, n, &pr) == REAC_CTRL_HEADAMP) {
		e->ha_seen = 1;
		e->ha_ch = pr.ch;
		e->ha_param = pr.param;
		e->ha_value = pr.value;
		e->ha_count++;
	}
}

/* One snapshot, replaced atomically. Resets the energy window. */
static void ear_report(struct ear *e, const char *path, unsigned long tx, int n_ch)
{
	char tmp[512];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "tx %lu\n", tx);
	fprintf(f, "rx_down %lu\n", e->rx_down);
	fprintf(f, "rx_other %lu\n", e->rx_other);
	fprintf(f, "rx_before_tx %lu\n", e->rx_before_tx);
	fprintf(f, "rx_bad_decode %lu\n", e->rx_bad_decode);
	fprintf(f, "last_len %zu\n", e->last_len);
	fprintf(f, "window_samples %lu\n", e->nsamp);
	for (int c = 0; c < n_ch && c < REAC_MAX_CHANNELS; c++) {
		/* dBFS of the window's RMS. A window with no samples in it prints
		 * "silent" rather than a number: an absent measurement must not read
		 * as a floor somebody could mistake for one. */
		if (e->nsamp == 0) {
			fprintf(f, "ch%d rms silent peak silent\n", c);
			continue;
		}
		double rms = sqrt(e->sumsq[c] / (double)e->nsamp);
		double db = rms > 0 ? 20.0 * log10(rms) : -999.0;
		double pdb = e->peak[c] > 0 ? 20.0 * log10(e->peak[c]) : -999.0;
		fprintf(f, "ch%d rms %.2f peak %.2f\n", c, db, pdb);
	}
	if (e->ha_seen)
		fprintf(f, "headamp ch %u param %u value %u count %lu\n",
		        e->ha_ch, e->ha_param, e->ha_value, e->ha_count);
	fclose(f);
	rename(tmp, path);
	memset(e->sumsq, 0, sizeof e->sumsq);
	memset(e->peak, 0, sizeof e->peak);
	e->nsamp = 0;
}

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
		fprintf(stderr, "usage: %s <iface> <src-mac> <channels> [fps] [report-file]\n",
		        argv[0]);
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
	const char *report = argc > 5 ? argv[5] : NULL;
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
	signal(SIGUSR1, on_usr1);

	/* THE EAR, on its own socket. Bound to 0x8819 it is handed RECEIVED frames only —
	 * locally generated ones go to ptype_all listeners — which is exactly the question
	 * here: what did the OTHER end put on this wire. The mode is the fabric's, not the
	 * box's: what arrives is a desk's 40-slot downstream whatever width we transmit. */
	struct ear ear;
	memset(&ear, 0, sizeof ear);
	ear.fd = -1;
	const struct reac_mode *dmode = reac_mode_for(fps * REAC_SAMPLES_PER_PKT);
	if (!dmode)
		dmode = &REAC_MODE_48K;
	if (report) {
		ear.fd = socket(AF_PACKET, SOCK_RAW, htons(0x8819));
		if (ear.fd < 0) {
			fprintf(stderr, "fake-box-master: RX socket: %s\n", strerror(errno));
			return 1;
		}
		struct sockaddr_ll rsll;
		memset(&rsll, 0, sizeof rsll);
		rsll.sll_family = AF_PACKET;
		rsll.sll_protocol = htons(0x8819);
		rsll.sll_ifindex = ifr.ifr_ifindex;
		if (bind(ear.fd, (struct sockaddr *)&rsll, sizeof rsll) < 0) {
			fprintf(stderr, "fake-box-master: RX bind: %s\n", strerror(errno));
			return 1;
		}
		int rcvbuf = 8 << 20;
		setsockopt(ear.fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
		fprintf(stderr, "fake-box-master: listening too — report -> %s\n", report);
	}

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

	uint8_t rxf[2048];
	struct timespec last_report = { 0, 0 };
	while (!stop_now) {
		/* DRAIN THE EAR FIRST, so a report written this iteration already accounts for
		 * everything that arrived during the last slot. Bounded: this loop owes a frame
		 * every period and must never be pulled off cadence by a busy wire. */
		if (ear.fd >= 0) {
			for (int i = 0; i < 32; i++) {
				ssize_t rn = recv(ear.fd, rxf, sizeof rxf, MSG_DONTWAIT);
				if (rn <= 0)
					break;
				ear_ingest(&ear, rxf, (size_t)rn, src, (unsigned long)sent, dmode);
			}
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double dt = (double)(now.tv_sec - last_report.tv_sec)
			          + (double)(now.tv_nsec - last_report.tv_nsec) / 1e9;
			if (dt > 0.3) {
				last_report = now;
				ear_report(&ear, report, (unsigned long)sent, n_ch > 8 ? n_ch : 8);
			}
		}
		if (tx_paused) {
			nanosleep(&period, NULL);
			continue;
		}
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
	fprintf(stderr, "fake-box-master: stopped after %ld frames (%ld master records), "
	        "heard %lu downstream frames back\n", sent, announces, ear.rx_down);
	if (report)
		ear_report(&ear, report, (unsigned long)sent, n_ch > 8 ? n_ch : 8);
	if (ear.fd >= 0)
		close(ear.fd);
	close(fd);
	return 0;
}
