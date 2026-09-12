// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* courtship-probe — the two ends of a wire on which nobody is ever granted.
 *
 * THE DEFECT IT MEASURES (reac-captures 85c1e97, m200-master-441k-2026-09-11): an
 * ungranted reac-pw slave courted a live M-200 for 93 s at 100 % duty. The desk keeps
 * exactly ONE box session per segment and its liveness is fed by any upstream of the
 * enrolled geometry whatever the source MAC, so our stream held the slot open, the desk
 * emitted zero scene transfers, and the real S-1608 rebooting beside us never enrolled.
 * With our slave absent the same desk released the slot 7.148 s after its box went quiet.
 * libreac's fix bounds the courtship (4 s) and then goes SILENT for 10 s
 * (libreac docs/design/specs/2026-09-12-bounded-ungranted-courtship.md).
 *
 * The libreac unit test drives the pure FSM. This drives the REAL ENGINE — reac_slave_open
 * + reac_slave_start, an AF_PACKET socket on a real interface — and measures the wire from
 * THE OTHER END of a veth pair, which is the only place the claim "we are off the wire" can
 * be read. A test that asked the slave what it decided would be asking the accused.
 *
 *   master <iface> <slave-mac> <fps> <secs>   broadcast a desk-shaped downstream at fps,
 *                                             a cfea announce 1/s, and NEVER a grant;
 *                                             timestamp every frame that arrives from
 *                                             <slave-mac> and print the census.
 *   slave  <iface> <mac> <rate> <secs>        the real slave engine, PHY up, for <secs>.
 *
 * PRESENCE BEFORE ABSENCE: the census leads with how many frames it HEARD. A silence
 * report from an ear that heard nothing at all is a broken ear, not a quiet slave, and the
 * harness refuses it on that number before it reads any gap.
 */
#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/transport/reac_ring.h>
#include <reac/transport/reac_slave.h>

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

static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
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

static int open_packet(const char *iface, int *ifindex)
{
	int fd = socket(AF_PACKET, SOCK_RAW, htons(0x8819));
	if (fd < 0) {
		fprintf(stderr, "courtship-probe: AF_PACKET: %s (need CAP_NET_RAW)\n",
		        strerror(errno));
		return -1;
	}
	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", iface);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) != 0) {
		fprintf(stderr, "courtship-probe: no interface '%s': %s\n", iface, strerror(errno));
		close(fd);
		return -1;
	}
	*ifindex = ifr.ifr_ifindex;
	return fd;
}

/* ---- the master that never grants ---------------------------------------- */

static int run_master(const char *iface, const uint8_t slave_mac[6], int fps, double secs)
{
	/* The desk's own width: reac_ctrl_build_flood_filler at 40 slots is 52 + 40*36 =
	 * 1492 B, the M-200's downstream length. Nothing here hand-rolls a frame. */
	enum { N_CH = 40 };
	static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	/* the M-200 of the capture */
	static const uint8_t SRC[6] = { 0x00, 0x40, 0xab, 0xc9, 0xcc, 0x03 };

	int ifindex = 0;
	int tx = open_packet(iface, &ifindex);
	if (tx < 0)
		return 77;
	struct sockaddr_ll sll;
	memset(&sll, 0, sizeof sll);
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(0x8819);
	sll.sll_ifindex = ifindex;
	sll.sll_halen = 6;
	memcpy(sll.sll_addr, BCAST, 6);

	/* A SECOND SOCKET FOR THE EAR. Bound to 0x8819 it is handed RECEIVED frames only,
	 * so what it counts is unambiguously the OTHER end's. */
	int rx = socket(AF_PACKET, SOCK_RAW, htons(0x8819));
	if (rx < 0) {
		close(tx);
		return 77;
	}
	struct sockaddr_ll rsll;
	memset(&rsll, 0, sizeof rsll);
	rsll.sll_family = AF_PACKET;
	rsll.sll_protocol = htons(0x8819);
	rsll.sll_ifindex = ifindex;
	if (bind(rx, (struct sockaddr *)&rsll, sizeof rsll) < 0) {
		fprintf(stderr, "courtship-probe: RX bind: %s\n", strerror(errno));
		close(tx); close(rx);
		return 77;
	}

	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);

	static float ch[N_CH][REAC_SAMPLES_PER_PKT];
	float *planar[N_CH];
	for (int c = 0; c < N_CH; c++)
		planar[c] = ch[c];

	uint8_t f[2048], rxf[2048];
	uint16_t counter = 0;
	long sent = 0, heard = 0, announces = 0;
	double t0 = now_s(), first = -1, last = -1, longest = 0, burst_end = -1;
	int bursts = 0, in_burst = 0;
	/* A gap only becomes a SILENCE once it is longer than the slave's own grid: the
	 * cold-connect emits one frame per master frame, so any real pause is orders of
	 * magnitude wider than a scheduling hiccup. 0.5 s is far above the one and far
	 * below the 10 s under test. */
	const double GAP_S = 0.5;

	struct timespec deadline;
	clock_gettime(CLOCK_MONOTONIC, &deadline);
	const long period_ns = 1000000000L / fps;

	while (!stop_now && now_s() - t0 < secs) {
		for (int i = 0; i < 64; i++) {
			ssize_t rn = recv(rx, rxf, sizeof rxf, MSG_DONTWAIT);
			if (rn <= 0)
				break;
			if (rn < 14 || memcmp(rxf + 6, slave_mac, 6) != 0)
				continue;   /* not the slave's */
			double t = now_s();
			if (first < 0) { first = t; in_burst = 1; bursts = 1; }
			if (last > 0 && t - last > GAP_S) {
				if (t - last > longest) longest = t - last;
				if (burst_end < 0) burst_end = last;   /* the FIRST burst's end */
				bursts++;
				(void)in_burst;
			}
			last = t;
			heard++;
		}

		size_t n = reac_ctrl_build_flood_filler(f, BCAST, SRC, counter++, N_CH,
		                                        planar, REAC_SAMPLES_PER_PKT);
		if (n == 0)
			break;
		/* THE cfea ANNOUNCE, ~1/s: what makes this peer a MASTER to the slave's
		 * classifier (reac_fsm.c is_master_frame) without ever granting anything.
		 * Same shape fake_box_master.c stamps, re-checksummed after the stamp. */
		if (sent % fps == fps / 4) {
			uint8_t *b = f + 16;
			static const uint8_t H[11] = { 0xcf, 0xea, 0xff, 0xff, 0x01, 0x00,
			                               0x01, 0x03, 0x0d, 0x01, 0x04 };
			memcpy(b, H, sizeof H);
			memcpy(b + 11, SRC, 6);
			b[17] = (uint8_t)N_CH; b[18] = 0x08; b[19] = 0x01;
			b[20] = 0x00; b[21] = 0x01;
			memset(b + 22, 0, 12);
			reac_ctrl_checksum_apply(f);
			announces++;
		}
		if (sendto(tx, f, n, 0, (struct sockaddr *)&sll, sizeof sll) < 0 &&
		    errno != ENOBUFS && errno != EAGAIN && errno != ENETDOWN) {
			fprintf(stderr, "courtship-probe: send: %s\n", strerror(errno));
			break;
		}
		sent++;
		deadline.tv_nsec += period_ns;
		while (deadline.tv_nsec >= 1000000000L) {
			deadline.tv_nsec -= 1000000000L;
			deadline.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
	}
	double span = now_s() - t0;
	/* A trailing silence counts too: the run may end inside a backoff. */
	if (last > 0 && now_s() - last > longest)
		longest = now_s() - last;

	printf("heard %ld\n", heard);
	printf("sent %ld\n", sent);
	printf("announces %ld\n", announces);
	printf("achieved_fps %.1f\n", sent / (span > 0 ? span : 1));
	printf("bursts %d\n", bursts);
	printf("first_burst_s %.3f\n", (first >= 0 && burst_end > 0) ? burst_end - first : -1.0);
	printf("first_tx_s %.3f\n", first >= 0 ? first - t0 : -1.0);
	printf("longest_silence_s %.3f\n", longest);
	printf("span_s %.3f\n", span);
	fflush(stdout);
	close(tx);
	close(rx);
	return 0;
}

/* ---- the real slave engine ------------------------------------------------ */

static int run_slave(const char *iface, const uint8_t mac[6], int rate, double secs)
{
	struct reac_ring ring;
	if (reac_ring_init(&ring, 16, 8192) != 0) {
		fprintf(stderr, "courtship-probe: ring init failed\n");
		return 1;
	}
	struct reac_slave slave;
	struct reac_slave_cfg cfg;
	memset(&cfg, 0, sizeof cfg);
	cfg.ifname = iface;
	cfg.box_channels = 16;     /* the S-1608 geometry of the capture */
	cfg.sample_rate = rate;
	cfg.src_mac = mac;
	cfg.tag = "[probe] ";
	if (reac_slave_open(&slave, &cfg, &ring) != 0) {
		fprintf(stderr, "courtship-probe: reac_slave_open failed on %s\n", iface);
		return 77;
	}
	if (reac_slave_start(&slave) != 0) {
		fprintf(stderr, "courtship-probe: reac_slave_start failed\n");
		reac_slave_close(&slave);
		return 1;
	}
	reac_slave_set_phy_up(&slave, 1);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	double t0 = now_s();
	while (!stop_now && now_s() - t0 < secs) {
		struct timespec s = { 0, 100000000L };
		nanosleep(&s, NULL);
	}
	reac_slave_stop(&slave);
	reac_slave_close(&slave);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 6) {
		fprintf(stderr,
		        "usage: %s master <iface> <slave-mac> <fps> <secs>\n"
		        "       %s slave  <iface> <mac> <rate> <secs>\n", argv[0], argv[0]);
		return 2;
	}
	uint8_t mac[6];
	if (parse_mac(argv[3], mac) != 0) {
		fprintf(stderr, "courtship-probe: '%s' is not a MAC\n", argv[3]);
		return 2;
	}
	int num = atoi(argv[4]);
	double secs = atof(argv[5]);
	if (num <= 0 || secs <= 0)
		return 2;
	if (strcmp(argv[1], "master") == 0)
		return run_master(argv[2], mac, num, secs);
	if (strcmp(argv[1], "slave") == 0)
		return run_slave(argv[2], mac, num, secs);
	fprintf(stderr, "courtship-probe: unknown mode '%s'\n", argv[1]);
	return 2;
}
