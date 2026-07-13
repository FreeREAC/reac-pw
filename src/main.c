// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac-pw — PipeWire-native REAC endpoint, MASTER or SLAVE role.
 *
 * A single libpipewire client that registers the 40-channel REAC source node
 * fed by a pcap replay (offline) or a live AF_PACKET 0x8819 socket, decoding
 * with the reac-aes67 plain-LE core and pushing samples through a lock-free
 * ring into the realtime process() callback. The node is a follower; PipeWire's
 * adapter resamples REAC -> graph (Tier-A clock bridge).
 *
 * REAC has no fixed master: any box can be the master and the rest slave to it.
 * openmixer must fit either role, selected with --role:
 *
 *   --role master (default) — WE drive the cdea/cfea establishment + own the
 *     clock (the SCHED_FIFO pacer at a fixed pps); a stagebox slaves to us. The
 *     downstream master TX is the reac:playback sink (--tx IFNAME).
 *   --role slave — an EXTERNAL master (a desk, or a box configured as master)
 *     drives the establishment; WE RESPOND and LOCK to the master's cadence (the
 *     master owns the clock — we never run our own pacer as the timing source).
 *     We RX the master's audio (reac:capture) and TX our input channels back
 *     upstream at the box's slots (reac_slave, --tx IFNAME = the REAC NIC).
 *
 * Both roles share the encoder/decoder + the PipeWire nodes; they differ only in
 * WHO drives the handshake + the clock (master drives; slave follows).
 *
 *   reac-pw --pcap capture.pcap [--rate 48000]
 *   reac-pw --live reac0 [--rate 96000] [--role master] [--tx reac0]
 *   reac-pw --live reac0 --role slave   --tx reac0      # slaved to a desk
 */

#include "reac_ring.h"
#include "reac_rx.h"
#include "reac_source_node.h"
#include "reac_sink_node.h"
#include "reac_slave.h"
#include "reac_role.h"

#include <pipewire/pipewire.h>
#include <reac/reac.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

static struct pw_main_loop *g_loop;

static void on_signal(void *data, int sig)
{
	(void)data; (void)sig;
	pw_main_loop_quit(g_loop);
}

/* Parse "aa:bb:cc:dd:ee:ff" into out[6]; returns 0, or -1 on malformed input. */
static int parse_mac(const char *s, uint8_t out[6])
{
	unsigned b[6];
	if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x",
	           &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
		return -1;
	for (int i = 0; i < 6; i++)
		out[i] = (uint8_t)b[i];
	return 0;
}

static void usage(const char *p)
{
	fprintf(stderr,
	  "usage: %s (--pcap FILE | --live IFNAME) [--role master|slave] [--rate R] [--tx IFNAME]\n"
	  "         [--mixer M] [--box MODEL[:NAME]] [--box-channels N] [--name NAME] [--src-mac M]\n"
	  "  --pcap FILE   replay a REAC capture (offline test, reuses pcap_source)\n"
	  "  --live IFNAME live AF_PACKET 0x8819 capture (reuses reac_capture; needs CAP_NET_RAW)\n"
	  "  --role R      master (default; WE drive the handshake + own the clock — a box\n"
	  "                slaves to us) | slave (an external master drives; we lock to its\n"
	  "                cadence + return our inputs upstream)\n"
	  "  --rate R      force the REAC sample rate (default: auto-detect on --live, 48000 on --pcap)\n"
	  "  --tx IFNAME   the REAC TX NIC: master role -> the reac:playback downstream sink;\n"
	  "                slave role -> the upstream return + handshake socket\n"
	  "  --box-channels N  slave role: our input width (even 2..40; 8=S-0808, 16=S-1608,\n"
	  "                32=S-4000S). Default 16. Sets the cold-connect/upstream/heartbeat width.\n"
	  "  --mixer M     master role: which Roland desk to impersonate (m200|m300|m5000;\n"
	  "                default m200). Sets the master MAC + console model; the grants\n"
	  "                are box-defined so any box locks to any profile.\n"
	  "  --box MODEL[:NAME]  master role: declare the box on THIS segment (one REAC/VLAN\n"
	  "                per box). MODEL is s0808|s1608|s4000s; sizes + labels reac:capture\n"
	  "                to its inputs and reac:playback to its outputs. Optional :NAME sets\n"
	  "                the openmixer label (default the model name).\n"
	  "  --name NAME   per-instance PipeWire node suffix (reac-capture.NAME /\n"
	  "                reac-playback.NAME) so one master per REAC VLAN/segment coexists.\n"
	  "  --src-mac M   our on-wire source MAC (aa:bb:cc:dd:ee:ff). Default: a Roland-OUI\n"
	  "                stand-in (master 00:40:ab:00:00:01, slave 00:40:ab:c4:80:41).\n"
	  "                Roland allocates ranges per device class (desks 00:40:ab:c9:xx:xx,\n"
	  "                boxes 00:40:ab:c4:xx:xx) — a box may validate its master's range\n", p);
}

int main(int argc, char **argv)
{
	struct reac_rx_cfg rxcfg = { .kind = REAC_RX_PCAP, .source = NULL, .forced_rate = 0,
	                             .pcap_realtime = 1 };
	const char *tx_if = NULL;
	enum reac_role role = REAC_ROLE_MASTER;   /* default master: preserves current behaviour */
	uint8_t src_mac[6];
	int src_mac_set = 0;
	int box_channels = REAC_SLAVE_BOX_CHANNELS_DEFAULT;  /* slave: our input width */
	int master_box_in = 0, master_box_out = 0; /* master: declared box widths (0 = 40 fabric) */
	int box_set = 0;                /* --box given (a master-role option)         */
	const char *box_label = NULL;   /* --box name: openmixer label for this box  */
	const char *inst_name = NULL;   /* --name: per-instance node suffix (one master/VLAN) */
	const struct reac_mixer_profile *mixer =
		reac_mixer_profile_by_name("m200");   /* master: which desk we impersonate */

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--pcap") && i + 1 < argc) {
			rxcfg.kind = REAC_RX_PCAP; rxcfg.source = argv[++i];
		} else if (!strcmp(argv[i], "--live") && i + 1 < argc) {
			rxcfg.kind = REAC_RX_LIVE; rxcfg.source = argv[++i];
		} else if (!strcmp(argv[i], "--rate") && i + 1 < argc) {
			/* Validate before it reaches the ring depth (sample_rate/4): a
			 * negative/garbage rate underflows to a huge depth, next_pow2
			 * overflows to a 0-slot ring with mask 0xFFFFFFFF, and the first
			 * write scribbles the heap. Accept only sane audio rates — the REAC
			 * world is the 44.1k/48k/88.2k/96k families. */
			int rate = atoi(argv[++i]);
			if (rate < 8000 || rate > 192000) {
				fprintf(stderr, "reac-pw: bad --rate '%s' (want 8000..192000 Hz; "
				        "REAC runs 44100/48000/88200/96000)\n", argv[i]);
				return 2;
			}
			rxcfg.forced_rate = rate;
		} else if (!strcmp(argv[i], "--tx") && i + 1 < argc) {
			tx_if = argv[++i];
		} else if (!strcmp(argv[i], "--src-mac") && i + 1 < argc) {
			if (parse_mac(argv[++i], src_mac) != 0) {
				fprintf(stderr, "reac-pw: bad --src-mac '%s' (want aa:bb:cc:dd:ee:ff)\n",
				        argv[i]);
				return 2;
			}
			src_mac_set = 1;
		} else if (!strcmp(argv[i], "--role") && i + 1 < argc) {
			if (reac_role_parse(argv[++i], &role) != 0) {
				fprintf(stderr, "reac-pw: unknown --role '%s' (master|slave)\n", argv[i]);
				return 2;
			}
		} else if (!strcmp(argv[i], "--mixer") && i + 1 < argc) {
			/* Master role: which Roland desk to impersonate (MAC + console model).
			 * The grants are box-defined, so a box locks to any profile. */
			mixer = reac_mixer_profile_by_name(argv[++i]);
			if (!mixer) {
				fprintf(stderr, "reac-pw: unknown --mixer '%s'; known:", argv[i]);
				for (int k = 0; reac_mixer_profile_at(k); k++)
					fprintf(stderr, " %s (%s)", reac_mixer_profile_at(k)->name,
					        reac_mixer_profile_at(k)->display);
				fprintf(stderr, "\n");
				return 2;
			}
		} else if (!strcmp(argv[i], "--box-model") && i + 1 < argc) {
			/* Slave role: pick a FIXED-matrix box model (the matrix is law when we
			 * are a stagebox). Selects the config-announce block, the ASCII name
			 * frame, and the width in one choice. */
			const struct reac_box_model *m = reac_box_model_by_token(argv[++i]);
			if (!m) {
				size_t n; const struct reac_box_model *t = reac_box_model_table(&n);
				fprintf(stderr, "reac-pw: unknown --box-model '%s'; known:", argv[i]);
				for (size_t k = 0; k < n; k++)
					fprintf(stderr, " %s (%s)", t[k].token, t[k].display);
				fprintf(stderr, "\n");
				return 2;
			}
			box_channels = m->in_ch;
		} else if (!strcmp(argv[i], "--box-channels") && i + 1 < argc) {
			box_channels = atoi(argv[++i]);
			if (box_channels < 2 || box_channels > REAC_MAX_CHANNELS || (box_channels & 1)) {
				fprintf(stderr, "reac-pw: --box-channels must be even, 2..%d "
				        "(e.g. 8 = S-0808, 16 = S-1608, 32 = S-4000S)\n", REAC_MAX_CHANNELS);
				return 2;
			}
		} else if (!strcmp(argv[i], "--box") && i + 1 < argc) {
			/* MASTER role: declare the box on THIS segment (one REAC/VLAN per box).
			 * Sizes reac:capture to the box's real inputs + reac:playback to its
			 * outputs, and labels them. Form: <model>[:name] e.g. s1608:Drums. */
			static char spec[64];
			snprintf(spec, sizeof spec, "%s", argv[++i]);
			char *colon = strchr(spec, ':');
			if (colon) { *colon = '\0'; box_label = colon + 1; }
			const struct reac_box_model *bm = reac_box_model_by_token(spec);
			if (!bm) {
				size_t nm; const struct reac_box_model *t = reac_box_model_table(&nm);
				fprintf(stderr, "reac-pw: unknown --box model '%s'; known:", spec);
				for (size_t k = 0; k < nm; k++) fprintf(stderr, " %s", t[k].token);
				fprintf(stderr, "\n");
				return 2;
			}
			master_box_in = bm->in_ch;
			master_box_out = bm->out_ch;
			if (!box_label) box_label = bm->display;
			box_set = 1;
		} else if (!strcmp(argv[i], "--name") && i + 1 < argc) {
			inst_name = argv[++i];   /* per-instance PW node suffix (multi-master) */
		} else {
			usage(argv[0]);
			return 2;
		}
	}
	if (!rxcfg.source) {
		usage(argv[0]);
		return 2;
	}
	if (reac_role_validate(role, tx_if != NULL) != 0) {
		fprintf(stderr, "reac-pw: --role slave needs --tx IFNAME (the REAC NIC for the "
		                "upstream return + handshake)\n");
		return 2;
	}
	/* --box declares a MASTER-role box (sizes + labels reac:capture/reac:playback
	 * to a real box on this segment); it is consumed only on the master path. As a
	 * slave it is a silent no-op whose label still leaks into our node description —
	 * reject the mix rather than mislead. A slave's own identity is --box-channels. */
	if (role == REAC_ROLE_SLAVE && box_set) {
		fprintf(stderr, "reac-pw: --box is a master-role option (it declares the box "
		                "this master serves); for slave identity use --box-channels "
		                "(e.g. --box-channels 16 = S-1608)\n");
		return 2;
	}

	/* The box infers its sample rate from the DESK MODEL we impersonate, NOT the
	 * packet cadence: rig-diffed (2026-07-13) an M-5000 vs an M-300 downstream —
	 * the rate signal is the desk IDENTITY (cfea console byte + ENROLL console
	 * byte: `01` = OHRCA/M-5000 => 96 kHz native, `00` = V-Mixer/M-200,M-300 =>
	 * 48 kHz only). There is no explicit 48000/96000 field. The downstream FRAME
	 * SHAPE itself does NOT vary by model (task #156 RE: the "1494-byte OHRCA
	 * frame" some captures show is a mirror/SPAN capture artifact — the 2 extra
	 * bytes are the standard Ethernet FCS, not a REAC field; see reac_tx.h /
	 * tests/test_reac_tx.c), so the only thing that changes for 96 kHz is the
	 * pacer's cadence (fps = rate/12, already rate-driven) and the console-
	 * identity bytes (already wired through --mixer's console_field).
	 *
	 * A V-Mixer desk has no wire rate field at all, so it is ALWAYS 48 kHz
	 * regardless of `--rate` (reac_mixer_resolve_rate) — that part of the
	 * original clamp is preserved. An OHRCA desk (M-5000) is native 96 kHz and
	 * honors `--rate`; see docs/MASTER-HARDWARE-VERIFY.md. */
	if (role == REAC_ROLE_MASTER) {
		int clamped = 0;
		int resolved = reac_mixer_resolve_rate(mixer, rxcfg.forced_rate, &clamped);
		if (clamped)
			fprintf(stderr, "reac-pw: master impersonating %s emits %d Hz "
			        "V-Mixer downstream only; --rate %d ignored (the box takes "
			        "its rate from the impersonated desk MODEL, not the "
			        "cadence) — use --mixer m5000 for 96 kHz.\n",
			        mixer->display, resolved, rxcfg.forced_rate);
		rxcfg.forced_rate = resolved;
	}

	/* The role picks which stream RX decodes (see DESIGN's role table): as
	 * MASTER our capture is a box's upstream return (its input channels,
	 * box-width braided frames); as SLAVE it is the master's 40-ch downstream
	 * broadcast. The wire carries both; the gate keeps them apart. */
	rxcfg.accept = (role == REAC_ROLE_MASTER) ? REAC_RX_ACCEPT_UPSTREAM
	                                          : REAC_RX_ACCEPT_DOWNSTREAM;

	pw_init(&argc, &argv);

	struct reac_ring ring;
	struct reac_rx rx;
	if (reac_rx_open(&rx, &rxcfg, &ring) != 0) {
		fprintf(stderr, "reac-pw: cannot open source '%s'\n", rxcfg.source);
		return 1;
	}
	fprintf(stderr, "reac-pw: recovered REAC rate = %d Hz (%d pps), rx stream = %s\n",
	        rx.sample_rate, rx.sample_rate / REAC_SAMPLES_PER_PKT,
	        rxcfg.accept == REAC_RX_ACCEPT_UPSTREAM
	          ? "box upstream return (box-width)" : "master downstream (40 ch)");

	g_loop = pw_main_loop_new(NULL);
	struct pw_loop *loop = pw_main_loop_get_loop(g_loop);
	pw_loop_add_signal(loop, SIGINT, on_signal, NULL);
	pw_loop_add_signal(loop, SIGTERM, on_signal, NULL);

	/* Master: expose the declared box's real inputs (16=S-1608, 8=S-0808); with no
	 * --box, the full 40-slot fabric. Slave: the source is the 40-ch downstream. */
	int src_ch = (role == REAC_ROLE_MASTER) ? master_box_in : 0;
	struct reac_source_node *src = reac_source_node_new(loop, &ring, &rx, rx.sample_rate,
	                                                    src_ch, inst_name, box_label,
	                                                    role == REAC_ROLE_MASTER);
	if (!src) {
		fprintf(stderr, "reac-pw: failed to create reac:capture node\n");
		return 1;
	}

	/* TX side: who drives the handshake + the clock depends on the role.
	 *   master -> reac:playback sink: WE encode the graph downstream + the pacer
	 *             drives the cdea/cfea grant + owns the clock (a box slaves to us).
	 *   slave  -> reac_slave engine: an external master drives; we lock to its
	 *             cadence + return our input channels upstream at the box's slots. */
	struct reac_sink_node *sink = NULL;
	struct reac_slave slave;
	int slave_open = 0;
	struct reac_ring tx_ring;
	int tx_ring_init = 0;

	if (tx_if && role == REAC_ROLE_MASTER) {
		/* Default master MAC = the impersonated desk's captured address; --src-mac
		 * overrides it. The mixer profile also sets the console-model byte. */
		const uint8_t *master_src = src_mac_set ? src_mac : mixer->mac;
		reac_ring_init(&tx_ring, REAC_MAX_CHANNELS, (uint32_t)(rx.sample_rate / 4));
		tx_ring_init = 1;
		struct reac_sink_cfg scfg = { .ifname = tx_if,
		                              .channels = master_box_out ? master_box_out
		                                                         : REAC_MAX_CHANNELS,
		                              .sample_rate = rx.sample_rate,
		                              .src_mac = master_src, .master_mac = NULL,
		                              .console_field = mixer->console_field,
		                              .inst = inst_name, .label = box_label };
		sink = reac_sink_node_new(loop, &tx_ring, &scfg); /* encodes + emits REAC */
		if (!sink)
			fprintf(stderr, "reac-pw: reac:playback sink not created "
			        "(TX socket on '%s' failed — need CAP_NET_RAW?)\n", tx_if);
		else
			fprintf(stderr, "reac-pw: MASTER role (impersonating %s) on '%s' — "
			        "event-driven establishment: probing until the box's "
			        "cold-connect (cdea 04 03) arrives; FSM/RX transcript on "
			        "stderr\n", mixer->display, tx_if);
	} else if (tx_if && role == REAC_ROLE_SLAVE) {
		/* The slave returns its OWN input channels (a box width) upstream. The PCM
		 * for them would come from a reac:return sink; for now the ring is the
		 * carrier and the slave emits silent/own-input FILLER until that sink is
		 * linked. The engine learns the master MAC from the wire — never set here. */
		static const uint8_t box_oui_mac[6] = { 0x00, 0x40, 0xab, 0xc4, 0x80, 0x41 };
		const uint8_t *slave_src = src_mac_set ? src_mac : box_oui_mac;
		reac_ring_init(&tx_ring, REAC_MAX_CHANNELS, (uint32_t)(rx.sample_rate / 4));
		tx_ring_init = 1;
		struct reac_slave_cfg slcfg = { .ifname = tx_if,
		                                .box_channels = box_channels,
		                                .sample_rate = rx.sample_rate,
		                                .src_mac = slave_src };
		if (reac_slave_open(&slave, &slcfg, &tx_ring) == 0) {
			slave_open = 1;
			if (reac_slave_start(&slave) == 0) {
				reac_slave_set_phy_up(&slave, 1);  /* PHY up: begin the establishment */
				fprintf(stderr, "reac-pw: SLAVE role on '%s' (%d-ch upstream return) — "
				        "responding to an external master, locked to its cadence\n",
				        tx_if, box_channels);
			} else {
				fprintf(stderr, "reac-pw: slave engine thread failed to start\n");
				reac_slave_close(&slave); slave_open = 0;
			}
		} else {
			fprintf(stderr, "reac-pw: slave engine not created (AF_PACKET on '%s' "
			        "failed — need CAP_NET_RAW?)\n", tx_if);
		}
	}

	if (reac_rx_start(&rx) != 0) {
		fprintf(stderr, "reac-pw: failed to start RX feeder\n");
		return 1;
	}

	pw_main_loop_run(g_loop);

	reac_rx_stop(&rx);
	reac_source_node_destroy(src);
	reac_sink_node_destroy(sink);
	if (slave_open) {
		reac_slave_stop(&slave);
		reac_slave_close(&slave);
	}
	if (tx_ring_init)
		reac_ring_free(&tx_ring);
	reac_rx_close(&rx);
	reac_ring_free(&ring);
	pw_main_loop_destroy(g_loop);
	pw_deinit();
	return 0;
}
