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
#include <reac/reac_master.h>
#include <reac/reac_sample.h>
#include <reac/reac_upstream.h>

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
#include "reac_facts_pw.h"   /* the protocol's numbers, from their one declaration */

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
	/* THE CONTROL-FRAME HISTOGRAM, by libreac's own classification, plus the raw
	 * announce and chanmap blocks as they last arrived. This is what makes two wires
	 * COMPARABLE: the joined one and an enrolled one are decoded by one tool, and
	 * "sending is always the same" is a diff of these lines rather than a reading of
	 * two journals (0.5.5). */
	unsigned long kind[24];
	uint8_t announce_blk[34]; int have_announce;
	uint8_t chanmap_blk[34];  int have_chanmap;
	/* THE LAST VALUE SEEN PER CELL, not the last record seen. An established master
	 * replays the COMPLETE head-amp scene at establishment, so "the last record" is
	 * whatever cell that sweep ended on and says nothing about the write under test
	 * (0.5.5: it read ch 7 sens 32 while the write was ch 0 sens 20). -1 = never seen. */
	short ha_val[REAC_HEADAMP_MAX_CH][REAC_HEADAMP_NPARAMS];
	unsigned long ha_n[REAC_HEADAMP_MAX_CH][REAC_HEADAMP_NPARAMS];
	int ha_seen;
	unsigned ha_ch, ha_param, ha_value;
	unsigned long ha_count;
	/* THE JOINING BOX'S UPSTREAM, and what we owe it back (0.5.6). A stagebox on M
	 * does grant: the ground-truth capture has the S-0808 echoing the joining
	 * S-1608's own cdea 04 03 records back inside its broadcast, byte for byte, 4 ms
	 * after the burst. That echo IS the grant, so the records are kept here for the
	 * TX side to hand back — without it nothing on this wire can ever establish. */
	uint8_t grant_q[4][34]; int grant_n;
	int grant_n_total;                 /* distinct records ever seen, for the proof */
	unsigned long up_frames, up_announce, up_join, up_hb;
	size_t up_len;
	double up_t_announce, up_t_join, up_t_hb_first, up_t_hb_last;
	double up_sq[REAC_MAX_CHANNELS], up_pk[REAC_MAX_CHANNELS];
	unsigned long up_ns; int up_nch;
	unsigned long flood_frames; double flood_t0, flood_t1; size_t flood_len;
	uint8_t announce_seen[34]; int have_announce_seen;
	int announce_ok; unsigned long announce_refused;
	/* THE TWO THINGS A REAL MASTER'S TIMING REFUSES (0.5.6-9). An announce that lands
	 * INSIDE the master's scene transfer is not answered - both granted joins waited for
	 * it to stop - and the emulator has to be able to say so, or a daemon that announces
	 * mid-transfer passes here and is refused on the rig. */
	int scene_running;                 /* the master is pushing its scene right now */
	unsigned long announce_in_scene;   /* announces that arrived while it was */
	/* THE STATE CLAIM, AND WHEN IT WAS MADE (0.5.6-5). A peer that carries the
	 * ESTABLISHED descriptor before we have granted it is telling us it is already
	 * linked, and the real S-0808 grants nothing to one that does — measured. The
	 * emulator was granting on the control bytes alone and would have passed every
	 * build that made this mistake, which is three of them. */
	unsigned long rx_frames_seen;      /* peer frames, for a frame INDEX */
	unsigned long desc_first_frame;    /* where 007a first appeared */
	unsigned long desc_req_frames;     /* fillers carrying REQUESTING (0x52) */
	int hb_after_burst;                /* a heartbeat arrived on the frame after the burst */
	int last_was_burst;
	unsigned long grant_frame;         /* where we granted */
	int desc_before_grant;             /* the refusal */
	unsigned long steady_bcast;   /* broadcast downstream after the announce */
};

static double ear_now(void)
{
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* THE PEER'S CONTROL PLANE, and the bar it has to clear to be granted.
 *
 * A REAL BOX ON M IS STRICT, and the rig proved it the hard way: 0.5.6-1 sent this chassis
 * four byte-perfect cold-connect bursts and was echoed nothing, lamp blinking, because its
 * config-announce declared the WRONG THING — selector 0x84 (the family of the box it was
 * talking TO) instead of 0x80, from a source MAC that was the NIC's own rather than a
 * Roland OUI. An emulator that grants anything with a cdea 04 03 in it would have passed
 * that build, so it grants only what the wire granted: an announce declaring 0x80 with a
 * self-consistent port table, from 00:40:ab. */
static void ear_control(struct ear *e, const uint8_t *f, size_t n, double t)
{
	struct reac_ctrl_parsed p;
	enum reac_ctrl_kind k = reac_ctrl_parse(f, n, &p);
	e->rx_frames_seen++;
	/* THE CONTROL AREA OF EVERY FRAME, whatever kind it is: the descriptor rides the
	 * carrier, so a burst or a filler claiming it counts the same. */
	{
		int desc = 0;
		for (int i = 18; i < 50; i++)
			if (f[i] != 0x00) { desc = 1; break; }
		/* THE REQUESTING STATE (0.5.6-10). A real slave's fillers carry 0x52 from its
		 * announce until the grant and 0x7a after; zeroing that window is refused by a
		 * real S-1608, so an emulator that ignores it would pass a daemon the rig will
		 * not. Counted here and asserted by the proof. */
		if (k == REAC_CTRL_FILLER && f[19] == 0x52)
			e->desc_req_frames++;
		if (desc && f[19] != 0x52 && k == REAC_CTRL_FILLER) {
			if (!e->desc_first_frame)
				e->desc_first_frame = e->rx_frames_seen;
			if (!e->grant_frame)
				e->desc_before_grant = 1;
		}
	}
	if (k == REAC_CTRL_CONFIG_ANNOUNCE) {
		if (!e->up_announce) e->up_t_announce = t;
		e->up_announce++;
		memcpy(e->announce_seen, f + 16, sizeof e->announce_seen);
		e->have_announce_seen = 1;
		/* block[6] is the model-family selector and block[10:22] the port-type
		 * table; a table of all-equal entries declares nothing. The OUI is Roland's
		 * or this is not gear a box has ever enrolled. */
		if (e->scene_running) {
			e->announce_in_scene++;
			e->announce_ok = 0;
			e->announce_refused++;
			return;      /* a box joining mid-transfer must not cancel it */
		}
		int sel_ok = f[16 + 6] == 0x80;
		int oui_ok = f[6] == 0x00 && f[7] == 0x40 && f[8] == 0xab;
		int tbl_ok = 0;
		for (int i = 11; i < 22; i++)
			if (f[16 + i] != f[16 + 10])
				tbl_ok = 1;
		e->announce_ok = sel_ok && oui_ok && tbl_ok;
		if (!e->announce_ok)
			e->announce_refused++;
	} else if (k == REAC_CTRL_GRANT) {
		if (!e->up_join) e->up_t_join = t;
		e->up_join++;
		/* NOT GRANTED UNTIL WE WERE TOLD WHAT IS ASKING, AND NOT TO A PEER THAT SAYS
		 * IT IS ALREADY LINKED. */
		/* ONE ECHO PER DISTINCT RECORD, which is what the real S-0808 does: replaying
		 * the S-1608's three distinct cold-connect records draws three echoes, and
		 * replaying the same file with its second record replaced by a copy of the
		 * first draws two. An emulator that echoed every record would have granted a
		 * burst that repeats itself, which is what ours sent until 0.5.6-7. */
		int already = 0;
		for (int i = 0; i < e->grant_n; i++)
			if (memcmp(e->grant_q[i], f + 16, 34) == 0)
				already = 1;
		if (e->announce_ok && !e->desc_before_grant && !already && e->grant_n < 4) {
			memcpy(e->grant_q[e->grant_n], f + 16, 34);
			e->grant_n++;
			e->grant_n_total++;
			if (!e->grant_frame)
				e->grant_frame = e->rx_frames_seen;
		}
	} else if (k == REAC_CTRL_BOX_HB) {
		/* THE PERIOD IS MEASURED FROM THE SECOND BEAT. The first is the one that
		 * follows the burst, before the grant — a statement of presence, not part of
		 * the cadence — and counting it stretches the measured period by the whole
		 * pre-grant wait (4.386 s against a ~1 s cadence, measured). */
		if (e->up_hb == 1) e->up_t_hb_first = t;
		e->up_hb++; e->up_t_hb_last = t;
		/* BOTH GRANTED BOXES HEARTBEAT ON THE FRAME AFTER THEIR BURST, before the
		 * grant. A peer that only beats once established is asking to be granted
		 * without having said it is there. */
		if (e->last_was_burst)
			e->hb_after_burst = 1;
	}
	if (k != REAC_CTRL_FILLER)
		e->last_was_burst = (k == REAC_CTRL_GRANT);
}

static void ear_ingest(struct ear *e, const uint8_t *f, size_t n, const uint8_t src[6],
                       unsigned long tx_so_far, const struct reac_mode *mode)
{
	if (n < REAC_HDR_COUNTER_OFF || f[REAC_ETHERTYPE_OFF] != (REAC_ETHERTYPE >> 8) || f[REAC_ETHERTYPE_OFF + 1] != (REAC_ETHERTYPE & 0xff))
		return;
	if (memcmp(f + 6, src, 6) == 0)
		return;                    /* our own egress, if the kernel ever echoes it */
	if (n != (size_t)REAC_FRAME_BYTES && n != (size_t)REAC_FRAME_BYTES_OHRCA) {
		/* NOT OURS TO DECODE AS A DOWNSTREAM — but on a wire we master it is the
		 * only interesting traffic there is: a box joining us speaks our own
		 * upstream geometry (0.5.6). Classify it, keep its records, and measure the
		 * audio it is sending into our outputs. */
		e->rx_other++;
		int nch = reac_upstream_channels(n);
		if (nch <= 0)
			return;
		double t = ear_now();
		int bcast = memcmp(f, "\xff\xff\xff\xff\xff\xff", 6) == 0;
		if (bcast) {
			if (!e->flood_frames) e->flood_t0 = t;
			e->flood_frames++; e->flood_t1 = t; e->flood_len = n;
			return;
		}
		e->up_frames++; e->up_len = n; e->up_nch = nch;
		ear_control(e, f, n, t);
		static uint8_t us24[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION];
		int uns = reac_upstream_decode(f, n, us24);
		if (uns > 0) {
			for (int c = 0; c < nch && c < REAC_MAX_CHANNELS; c++)
				for (int i = 0; i < uns; i++) {
					float v = reac_s24le_to_f32(&us24[(size_t)(c * uns + i) * REAC_RESOLUTION]);
					e->up_sq[c] += (double)v * v;
					double a = v < 0 ? -(double)v : (double)v;
					if (a > e->up_pk[c]) e->up_pk[c] = a;
				}
			e->up_ns += (unsigned long)uns;
		}
		return;
	}
	e->rx_down++;
	e->last_len = n;
	if (tx_so_far == 0)
		e->rx_before_tx++;
	/* THE MIXER'S OWN FRAME IS 1492 B (0.5.6, operator ruling: "mixer always sends 40ch,
	 * boxes send their width only"), so the enrolment we are being asked for arrives
	 * inside these and not in a box-shaped upstream. Same classification, same grant
	 * queue — what changed is the geometry it rides in. */
	{
		double t = ear_now();
		/* THE FLOOD IS WHAT COMES BEFORE THE ANNOUNCE. A mixer's steady-state
		 * downstream is broadcast too (0.5.6), so counting every broadcast frame
		 * as "the presence flood" would report the whole run as one — measured:
		 * 21925 frames over 12.6 s where the flood is bounded at 5460. The announce
		 * is the boundary the box itself uses: the peer stops flooding and speaks. */
		if (memcmp(f, "\xff\xff\xff\xff\xff\xff", 6) == 0) {
			if (!e->up_announce) {
				if (!e->flood_frames) e->flood_t0 = t;
				e->flood_frames++; e->flood_t1 = t; e->flood_len = n;
			} else {
				e->steady_bcast++;
			}
		} else {
			e->up_frames++; e->up_len = n;
		}
		ear_control(e, f, n, t);
	}

	/* THE AUDIO, through the same decoder the daemon's own capture path uses. */
	static uint8_t s24[REAC_MAX_CHANNELS * REAC_SAMPLES_PER_PKT * REAC_RESOLUTION];
	int ns = reac_decode(f, REAC_FRAME_BYTES, mode, s24);
	if (ns < 0) {
		e->rx_bad_decode++;
	} else {
		for (int c = 0; c < mode->n_channels && c < REAC_MAX_CHANNELS; c++)
			for (int i = 0; i < ns; i++) {
				float v = reac_s24le_to_f32(&s24[(size_t)(c * ns + i) * REAC_RESOLUTION]);
				e->sumsq[c] += (double)v * (double)v;
				double a = v < 0 ? -(double)v : (double)v;
				if (a > e->peak[c])
					e->peak[c] = a;
			}
		e->nsamp += (unsigned long)ns;
	}

	/* THE CONTROL BLOCK, through libreac's own classifier. */
	struct reac_ctrl_parsed pr;
	enum reac_ctrl_kind kind = reac_ctrl_parse(f, n, &pr);
	if ((unsigned)kind < 24)
		e->kind[kind]++;
	if (kind == REAC_CTRL_MASTER_ANNOUNCE) {
		memcpy(e->announce_blk, f + 16, sizeof e->announce_blk);
		e->have_announce = 1;
	} else if (kind == REAC_CTRL_MASTER_HB) {
		memcpy(e->chanmap_blk, f + 16, sizeof e->chanmap_blk);
		e->have_chanmap = 1;
	}
	if (kind == REAC_CTRL_HEADAMP) {
		e->ha_seen = 1;
		e->ha_ch = pr.ch;
		e->ha_param = pr.param;
		e->ha_value = pr.value;
		e->ha_count++;
		if (pr.ch < REAC_HEADAMP_MAX_CH && pr.param < REAC_HEADAMP_NPARAMS) {
			e->ha_val[pr.ch][pr.param] = (short)pr.value;
			e->ha_n[pr.ch][pr.param]++;
		}
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
	fprintf(f, "flood frames %lu len %zu secs %.3f\n", e->flood_frames, e->flood_len,
	        e->flood_frames ? e->flood_t1 - e->flood_t0 : 0.0);
	fprintf(f, "announce ok %d refused %lu in_scene %lu\n",
	        e->announce_ok, e->announce_refused, e->announce_in_scene);
	fprintf(f, "steady bcast %lu\n", e->steady_bcast);
	fprintf(f, "hb_after_burst %d\n", e->hb_after_burst);
	fprintf(f, "descriptor first %lu grant %lu before_grant %d requesting %lu\n",
	        e->desc_first_frame, e->grant_frame, e->desc_before_grant, e->desc_req_frames);
	fprintf(f, "distinct records %d\n", e->grant_n_total);
	if (e->have_announce_seen) {
		fprintf(f, "announceblk ");
		for (size_t i = 0; i < sizeof e->announce_seen; i++)
			fprintf(f, "%02x", e->announce_seen[i]);
		fprintf(f, "\n");
	}
	fprintf(f, "up frames %lu len %zu ch %d announce %lu join %lu hb %lu\n",
	        e->up_frames, e->up_len, e->up_nch, e->up_announce, e->up_join, e->up_hb);
	if (e->up_announce && e->up_join)
		fprintf(f, "up order announce_to_join %.3f\n", e->up_t_join - e->up_t_announce);
	if (e->up_hb > 2)
		fprintf(f, "up hb_period %.3f\n",
		        (e->up_t_hb_last - e->up_t_hb_first) / (double)(e->up_hb - 2));
	for (int c = 0; c < e->up_nch && c < REAC_MAX_CHANNELS; c++) {
		if (!e->up_ns) break;
		double r = sqrt(e->up_sq[c] / (double)e->up_ns);
		fprintf(f, "upch %d rms %.2f peak %.2f\n", c,
		        r > 0 ? 20.0 * log10(r) : -999.0,
		        e->up_pk[c] > 0 ? 20.0 * log10(e->up_pk[c]) : -999.0);
	}
	memset(e->up_sq, 0, sizeof e->up_sq);
	memset(e->up_pk, 0, sizeof e->up_pk);
	e->up_ns = 0;
	for (int k = 0; k < 24; k++)
		if (e->kind[k])
			fprintf(f, "kind %s %lu\n", reac_ctrl_kind_name(k), e->kind[k]);
	if (e->have_announce) {
		fprintf(f, "announce ");
		for (size_t i = 0; i < sizeof e->announce_blk; i++)
			fprintf(f, "%02x", e->announce_blk[i]);
		fprintf(f, "\n");
	}
	if (e->have_chanmap) {
		fprintf(f, "chanmap ");
		for (size_t i = 0; i < sizeof e->chanmap_blk; i++)
			fprintf(f, "%02x", e->chanmap_blk[i]);
		fprintf(f, "\n");
	}
	if (e->ha_seen)
		fprintf(f, "headamp ch %u param %u value %u count %lu\n",
		        e->ha_ch, e->ha_param, e->ha_value, e->ha_count);
	for (int c = 0; c < REAC_HEADAMP_MAX_CH; c++)
		for (int q = 0; q < REAC_HEADAMP_NPARAMS; q++)
			if (e->ha_val[c][q] >= 0)
				fprintf(f, "headampcell %d %d %d %lu\n",
				        c, q, e->ha_val[c][q], e->ha_n[c][q]);
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
	/* A MASTER THAT CALLS, and one that is silent. The S-1608 in master mode sends a
	 * `cfea` announce about once a second and pushes a scene transfer; the S-0808 sends
	 * neither. A joining box floods only at the silent one, and waits for the other's
	 * transfer to stop - so both kinds have to exist here or half the rule is untested. */
	int announcing = (argc > 6 && strcmp(argv[6], "announcing") == 0);
	/* fps 0 IS A MODE, NOT A REFUSAL (0.5.5): transmit nothing and only listen. It is
	 * how the SAME decoder is pointed at a wire somebody else's box is enrolled on, so
	 * the two downstreams can be diffed by one tool instead of two readings. */
	int listen_only = (fps == 0);
	if (n_ch <= 0 || n_ch >= REAC_MAX_CHANNELS || fps < 0) {
		fprintf(stderr, "fake-box-master: a box width is 1..%d channels and fps >= 0 "
		        "(0 = listen only)\n", REAC_MAX_CHANNELS - 1);
		return 2;
	}
	if (listen_only)
		fps = 2000;   /* the drain/report cadence only; nothing is transmitted */

	int fd = socket(AF_PACKET, SOCK_RAW, htons(REAC_ETHERTYPE));
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
	sll.sll_protocol = htons(REAC_ETHERTYPE);
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
	for (int c = 0; c < REAC_HEADAMP_MAX_CH; c++)
		for (int q = 0; q < REAC_HEADAMP_NPARAMS; q++)
			ear.ha_val[c][q] = -1;
	const struct reac_mode *dmode = reac_mode_for(fps * REAC_SAMPLES_PER_PKT);
	if (!dmode)
		dmode = &REAC_MODE_48K;
	/* THE EAR IS NOT A REPORTING OPTION, IT IS HALF OF BEING A BOX (0.5.6). A stagebox on
	 * M grants: it echoes a joining box's own cdea 04 03 records back inside its
	 * broadcast, and a peer that cannot hear cannot grant — so a run without a report
	 * file would silently be a box nothing can ever enrol with, which is exactly the
	 * `probing` a console must not see. The socket is always opened; `report` only says
	 * whether a snapshot is also written. */
	{
		ear.fd = socket(AF_PACKET, SOCK_RAW, htons(REAC_ETHERTYPE));
		if (ear.fd < 0) {
			fprintf(stderr, "fake-box-master: RX socket: %s\n", strerror(errno));
			return 1;
		}
		struct sockaddr_ll rsll;
		memset(&rsll, 0, sizeof rsll);
		rsll.sll_family = AF_PACKET;
		rsll.sll_protocol = htons(REAC_ETHERTYPE);
		rsll.sll_ifindex = ifr.ifr_ifindex;
		if (bind(ear.fd, (struct sockaddr *)&rsll, sizeof rsll) < 0) {
			fprintf(stderr, "fake-box-master: RX bind: %s\n", strerror(errno));
			return 1;
		}
		int rcvbuf = 8 << 20;
		setsockopt(ear.fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
		fprintf(stderr, "fake-box-master: listening too (grants what joins us)%s%s\n",
		        report ? " — report -> " : "", report ? report : "");
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
	/* THE ANNOUNCE BLOCK COMES FROM libreac's GENERATOR, with the two fields a BOX
	 * in master mode measurably differs from a desk in overwritten and cited.
	 *
	 * It used to be hand-written here — the cfea head, then `b[18]=0x08;
	 * b[19]=0x01; b[20]=0x00; b[21]=0x01` — which is a second copy of the protocol
	 * in a test, and two of those four bytes were wrong whatever the run:
	 * reac-captures/analysis/2026-09-13-announce-bytes-and-headamp-base.md §2 reads
	 * all 17 040 announces in the corpus and settles [19] as the PACE CODE (so a
	 * hard 0x01 announces 96 kHz at every rate — a defect it names in our own
	 * emitters) and [20:22] as the enrolled-box count, u2 big-endian (so a hard 1
	 * announced an enrolment before anything had been granted).
	 *
	 * What the generator writes and we keep: the announce header, our MAC, the idle
	 * 0x08 at [18] — the "upstream width IN FORCE", which is what an S-1608 in
	 * master mode really announced while granting an S-4000S
	 * (box-to-box-2026-09-13) — and reac_pace_code(fps) at [19].
	 *
	 * What we overwrite: [17]. A DESK writes the fixed 0x28 there (the 40-slot
	 * downstream fabric); a box in master mode writes its OWN declared input width
	 * — 0x10 measured on the S-1608, 0x20 on the S-4000S, same analysis §2. This
	 * emulator is a box, so it declares n_ch. */
	static struct reac_master ann;
	struct reac_console_cfg ann_cfg = { .out_channels = REAC_BOX_S1608_OUT,
	                                    .console_field = reac_pace_code(fps) };
	reac_master_init(&ann, src, &ann_cfg, fps);
	ann.announce_blk[17] = (uint8_t)n_ch;

	long sent = 0, announces = 0, granted = 0;
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
			if (report && dt > 0.3) {
				last_report = now;
				ear_report(&ear, report, (unsigned long)sent, n_ch > REAC_BOX_S0808_IN ? n_ch : REAC_BOX_S0808_IN);
			}
		}
		if (tx_paused || listen_only) {
			nanosleep(&period, NULL);
			continue;
		}
		size_t n = reac_ctrl_build_flood_filler(f, BCAST, src, counter++, n_ch,
		                                        planar, REAC_SAMPLES_PER_PKT);
		if (n == 0)
			break;
		/* THE ANNOUNCING MASTER'S OWN CONTROL PLANE (0.5.6-9): a bounded scene
		 * transfer over the first second, then a `cfea` announce about once a
		 * second. Both are stamped over the filler's control block and
		 * re-checksummed, exactly as the head-amp record below is. */
		if (announcing) {
			/* THE TRANSFER REPEATS UNTIL IT IS ANSWERED (spec/reac.ksy: four
			 * complete bodies in one capture, ten in another, all at the same
			 * period). A single burst would let an unguarded announce miss it by
			 * luck, which is not a test of anything: 0.5 s of transfer every
			 * 2 s, so a daemon that announces on its own clock lands inside one
			 * and a daemon that waits for quiet always has a window. */
			ear.scene_running = ((sent / (fps / 2)) % 4) == 0 && sent > fps / 4;
			if (ear.scene_running) {
				static const uint8_t SCENE[6] = { 0xcd, 0xea, 0x01, 0x00,
				                                  0x00, 0x1a };
				memcpy(f + 16, SCENE, sizeof SCENE);
				memset(f + 22, 0, 28);
				reac_ctrl_checksum_apply(f);
				announces++;
				goto send;
			}
			if (!ear.scene_running && sent % fps == fps / 4) {
				/* cfea: the master naming itself, its fabric and its width -
				 * the shape a real S-1608 in master mode broadcasts. The
				 * generated block, plus the one field that moves during a run:
				 * the enrolled-box count rises when a grant has actually left
				 * the wire, not when the peer was recognized (the M-200
				 * timeline, m200-enrol-441k-2026-09-13/analysis.md). */
				uint8_t *b = f + 16;
				memcpy(b, ann.announce_blk, sizeof ann.announce_blk);
				b[21] = granted > 0 ? 0x01 : 0x00;
				reac_ctrl_checksum_apply(f);
				announces++;
				goto send;
			}
		}

		/* AND WE GRANT (0.5.6). A stagebox on M is not deaf: the ground-truth capture
		 * has the S-0808 echoing the joining S-1608's own cdea 04 03 records back
		 * inside its BROADCAST, byte for byte, 4 ms after the burst — that echo IS
		 * the grant, and without it nothing on this wire can ever establish. One
		 * queued record per frame, stamped over the filler's control block and
		 * re-checksummed, exactly as the head-amp record below is. */
		if (ear.grant_n > 0) {
			memcpy(f + 16, ear.grant_q[0], 34);
			memmove(ear.grant_q[0], ear.grant_q[1], sizeof ear.grant_q[0] * 3);
			ear.grant_n--;
			reac_ctrl_checksum_apply(f);
			granted++;
			goto send;
		}
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
send:
		if (sendto(fd, f, n, 0, (struct sockaddr *)&sll, sizeof sll) < 0 &&
		    errno != ENOBUFS && errno != EAGAIN && errno != ENETDOWN) {
			fprintf(stderr, "fake-box-master: send: %s\n", strerror(errno));
			break;
		}
		sent++;
		nanosleep(&period, NULL);
	}
	fprintf(stderr, "fake-box-master: stopped after %ld frames (%ld master records, "
	        "%ld grants echoed), heard %lu downstream / %lu upstream frames back\n",
	        sent, announces, granted, ear.rx_down, ear.up_frames);
	if (report)
		ear_report(&ear, report, (unsigned long)sent, n_ch > REAC_BOX_S0808_IN ? n_ch : REAC_BOX_S0808_IN);
	if (ear.fd >= 0)
		close(ear.fd);
	close(fd);
	return 0;
}
