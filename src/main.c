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
#include "reac_mac.h"
#include "reac_ctrl.h"        /* enum reac_headamp_param, REAC_HEADAMP_SENS_MAX */
#include "reac_headamp_tx.h"  /* struct reac_headamp_setting */

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

/* Parse "CH:PARAM:VALUE" (a master-role --headamp arg) into *out. CH is the WIRE
 * channel (0..REAC_HEADAMP_MAX_CH-1); PARAM is phantom|pad|sens; VALUE is 0/1 for
 * phantom|pad and the raw SENS code 0..0x37 for sens. Returns 0, or -1 if
 * malformed / out of range. */
static int parse_headamp(const char *s, struct reac_headamp_setting *out)
{
	unsigned ch, val;
	char pstr[16];
	if (sscanf(s, "%u:%15[^:]:%u", &ch, pstr, &val) != 3)
		return -1;
	uint8_t param;
	if (!strcmp(pstr, "phantom"))
		param = REAC_HEADAMP_PHANTOM;
	else if (!strcmp(pstr, "pad"))
		param = REAC_HEADAMP_PAD;
	else if (!strcmp(pstr, "sens"))
		param = REAC_HEADAMP_SENS;
	else
		return -1;
	if (ch >= REAC_HEADAMP_MAX_CH)
		return -1;
	if (param == REAC_HEADAMP_SENS) {
		if (val > REAC_HEADAMP_SENS_MAX)
			return -1;
	} else if (val > 1) {
		return -1;
	}
	out->ch = (uint8_t)ch;
	out->param = param;
	out->value = (uint8_t)val;
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
	  "  --headamp CH:PARAM:VALUE  master role, repeatable: a per-channel head-amp\n"
	  "                command the master re-asserts to the box (declarative/DMX).\n"
	  "                CH = wire channel 0..39; PARAM = phantom|pad|sens; VALUE = 0/1\n"
	  "                for phantom|pad, 0..55 raw SENS code for sens. RIG-GATED.\n"
	  "  --src-mac M   our on-wire source MAC (aa:bb:cc:dd:ee:ff). Default: master role\n"
	  "                uses the impersonated desk's MAC; slave role uses the Roland OUI\n"
	  "                (00:40:ab) + the last 3 bytes of the --tx NIC's own hardware\n"
	  "                address, so it stays Roland-OUI-compatible yet can never collide\n"
	  "                with a real box (e.g. an S-1608 at 00:40:ab:c4:80:41).\n"
	  "                Roland allocates ranges per device class (desks 00:40:ab:c9:xx:xx,\n"
	  "                boxes 00:40:ab:c4:xx:xx) — a box may validate its master's range\n"
	  "environment (see docs/ENV-KNOBS.md; unset = default behavior, byte-identical):\n"
	  "  REACPW_GRANT_DWELL_S=N  master role: hold the recognized-but-ungranted dwell\n"
	  "                for N whole seconds before the grant burst (default: the built-in\n"
	  "                ~1.6 s dwell; a real M-200 holds a cold box ~27 s)\n"
	  "  REAC_DEBUG=1  opt-in RX/source telemetry on stderr (~every 2 s: frame/dup/gap\n"
	  "                counters, ring fill, active channels)\n"
	  "  REACPW_CLOCK_FOLLOW=1  master role: DISCIPLINE the TX cadence to the best\n"
	  "                available clock reference (NIC/external PHC > a hardware-driven\n"
	  "                PipeWire graph clock > the box's counter slope) instead of\n"
	  "                free-running on CLOCK_MONOTONIC. The period is steered\n"
	  "                continuously and bounded; the phase is never stepped. The\n"
	  "                reference in use is printed on every change, and with none\n"
	  "                available we free-run and SAY so. Unset = today's behaviour,\n"
	  "                byte- and timing-identical. RIG-GATED.\n", p);
}

/* MASTER autodetect (no --box): a main-loop watcher that polls the box the pacer
 * recognized on the wire and (re)sizes the reac-capture / reac-playback nodes to its
 * real widths. Node create/destroy MUST run on the main/loop thread; recognition runs
 * in the RT pacer thread, which hands the model over via the pacer's atomic
 * recognized_box (read here through reac_sink_node_recognized_box) — so no pw_* call is
 * ever made from the pacer thread. The library owns the node lifecycle: this reads the
 * width + label and calls the two ensure() entry points, nothing more. */
struct autodetect_ctx {
	struct reac_source_node    **src;   /* main's source slot (created/rebuilt here) */
	struct reac_sink_node       *sink;  /* the master engine (owns the recognizer)   */
	struct reac_source_node_cfg  scfg;  /* stable reac-capture create args           */
	const struct reac_box_model *last;  /* last model acted on (edge-detects changes) */
};

static void on_autodetect_timer(void *data, uint64_t expirations)
{
	(void)expirations;
	struct autodetect_ctx *c = data;
	const struct reac_box_model *bm = reac_sink_node_recognized_box(c->sink);
	if (!bm || bm == c->last)
		return;   /* nothing recognized yet, or the same model as last poll */
	c->last = bm;
	/* Everything derived from the recognized in_ch/out_ch — no per-model branches. */
	if (reac_source_node_ensure(c->src, &c->scfg, bm->in_ch, bm->display) != 0)
		fprintf(stderr, "reac-pw: could not size reac-capture to %d ch (%s)\n",
		        bm->in_ch, bm->display);
	if (reac_sink_node_ensure(c->sink, bm->out_ch, bm->display) != 0)
		fprintf(stderr, "reac-pw: could not size reac-playback to %d ch (%s)\n",
		        bm->out_ch, bm->display);
	fprintf(stderr, "reac-pw: autodetected %s -> reac-capture %d in / reac-playback "
	        "%d out\n", bm->display, bm->in_ch, bm->out_ch);
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
	/* --headamp CH:PARAM:VALUE (master role, repeatable): the per-channel head-amp
	 * DMX table the master re-asserts to the box (task #155). At most one cell per
	 * (channel,param); the table set() overwrites a repeat. */
	struct reac_headamp_setting headamps[REAC_HEADAMP_MAX_CH * REAC_HEADAMP_NPARAMS];
	int n_headamps = 0;
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
		} else if (!strcmp(argv[i], "--headamp") && i + 1 < argc) {
			/* MASTER role: one per-channel head-amp cell the master re-asserts to
			 * the box (declarative/DMX). Repeatable; the table dedups per cell. */
			if (n_headamps >= (int)(sizeof headamps / sizeof headamps[0])) {
				fprintf(stderr, "reac-pw: too many --headamp settings (max %d)\n",
				        (int)(sizeof headamps / sizeof headamps[0]));
				return 2;
			}
			if (parse_headamp(argv[++i], &headamps[n_headamps]) != 0) {
				fprintf(stderr, "reac-pw: bad --headamp '%s' (want "
				        "CH:phantom|pad|sens:VALUE; CH 0..%d wire channel; "
				        "VALUE 0/1 for phantom|pad, 0..%d raw code for sens)\n",
				        argv[i], REAC_HEADAMP_MAX_CH - 1, REAC_HEADAMP_SENS_MAX);
				return 2;
			}
			n_headamps++;
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
	/* --headamp drives the box's preamps — only the MASTER commands them; as a slave
	 * WE are the box and receive them (surfaced in the RX log). Reject the mix. */
	if (role == REAC_ROLE_SLAVE && n_headamps > 0) {
		fprintf(stderr, "reac-pw: --headamp is a master-role option (the master commands "
		                "the box's preamps); a slave receives head-amp records, it does "
		                "not send them\n");
		return 2;
	}

	/* SAMPLE RATE — the master chooses it; the box follows.
	 *
	 * On a real Roland desk the operator selects the REAC rate from a menu. The
	 * desk drives the segment at that rate and every stagebox locks to it — a box
	 * has no rate setting of its own. reac-pw is the master here, so `--rate` is
	 * the same choice, and it is honoured as given.
	 *
	 * A previous revision of this comment claimed the box infers its rate from the
	 * DESK IDENTITY (console byte 01 = OHRCA => 96 kHz, 00 = V-Mixer => 48 kHz
	 * only) and clamped --rate to match. That was an inference, never
	 * demonstrated, and it is wrong: the identity byte says which desk we
	 * impersonate, not which rate the operator picked.
	 *
	 * Beware this paragraph's history — the same block also asserted the upstream
	 * "+2 bytes" were the Ethernet FCS, falsified 2026-07-25 (a real OHRCA CRC-16;
	 * docs/OHRCA-UPSTREAM-DUPLICATE-FRAMES.md, fixtures UP32A/UP32B). Two wrong
	 * claims from one comment: state what is measured, mark the rest open (#73).
	 *
	 * Cadence is fps = rate/12 at every rate — 12 samples per frame is invariant,
	 * so a higher rate sends the same frames more often, nothing else changes. */

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

	/* The reac-capture source is created AFTER the TX side, because whether to DEFER
	 * it depends on whether a recognizer (the master pacer) exists. In pure autodetect
	 * (master + a live TX pacer) it is deferred: nothing plugged -> nothing in the
	 * graph, and the node appears sized to the box the moment it is recognized (the
	 * autodetect timer below). Every other mode (slave, or pcap / no-TX master) has no
	 * recognizer, so the node is created at its startup width. The cfg bundles the
	 * process-lifetime constants so a later resize needs only the width + label. */
	struct reac_source_node *src = NULL;
	struct reac_source_node_cfg src_cfg = {
		.loop = loop, .ring = &ring, .rx = &rx, .sample_rate = rx.sample_rate,
		.inst = inst_name, .master_role = (role == REAC_ROLE_MASTER),
	};

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
		                              .inst = inst_name, .label = box_label,
		                              .headamps = n_headamps ? headamps : NULL,
		                              .n_headamps = n_headamps,
		                              /* #75: default OFF -> the pacer free-runs on
		                               * CLOCK_MONOTONIC exactly as it always has. */
		                              .clock_follow = getenv("REACPW_CLOCK_FOLLOW") != NULL };
		sink = reac_sink_node_new(loop, &tx_ring, &scfg); /* encodes + emits REAC */
		if (!sink)
			fprintf(stderr, "reac-pw: reac:playback sink not created "
			        "(TX socket on '%s' failed — need CAP_NET_RAW?)\n", tx_if);
		else {
			fprintf(stderr, "reac-pw: MASTER role (impersonating %s) on '%s' — "
			        "event-driven establishment: probing until the box's "
			        "cold-connect (cdea 04 03) arrives; FSM/RX transcript on "
			        "stderr\n", mixer->display, tx_if);
			if (n_headamps)
				fprintf(stderr, "reac-pw: head-amp DMX send armed — %d cell(s), "
				        "re-asserted once established (RIG-GATED: verify 48V at the "
				        "XLR pins)\n", n_headamps);
		}
	} else if (tx_if && role == REAC_ROLE_SLAVE) {
		/* The slave returns its OWN input channels (a box width) upstream. The PCM
		 * for them would come from a reac:return sink; for now the ring is the
		 * carrier and the slave emits silent/own-input FILLER until that sink is
		 * linked. The engine learns the master MAC from the wire — never set here. */
		uint8_t box_mac[6];
		if (src_mac_set) {
			memcpy(box_mac, src_mac, 6);
		} else if (reac_mac_default_src(tx_if, box_mac) != 0) {
			/* NIC hwaddr unreadable — the Roland-OUI + fixed-fallback host part is
			 * still on-wire safe (outside the box/desk device-class ranges), but note
			 * it so an ambiguous capture is explained. */
			fprintf(stderr, "reac-pw: could not read %s hardware address for the box "
			        "MAC host part; using the fixed fallback\n", tx_if);
		}
		const uint8_t *slave_src = box_mac;
		fprintf(stderr, "reac-pw: slave box source MAC = "
		        "%02x:%02x:%02x:%02x:%02x:%02x%s\n",
		        box_mac[0], box_mac[1], box_mac[2], box_mac[3], box_mac[4], box_mac[5],
		        src_mac_set ? " (--src-mac override)"
		                    : " (Roland OUI + this NIC's host part; --src-mac overrides)");
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

	/* Now that we know whether a recognizer exists (master + a live TX pacer), either
	 * DEFER the box nodes to autodetect or expose the source at its startup width. */
	struct autodetect_ctx adc = {0};
	struct spa_source *ad_timer = NULL;
	/* #75: the RX feeder is the BOX clock reference's measurement source — it already
	 * tracks the box's counter slope and publishes a filtered ppm error. The sink's
	 * existing 200 ms timer forwards it to the pacer's discipline. Wired
	 * unconditionally; it is only ever read when clock following is enabled. */
	if (sink)
		reac_sink_node_set_rate_source(sink, &rx);
	if (role == REAC_ROLE_MASTER && sink) {
		/* Pure autodetect: the pacer recognizes the box on the wire; a 200 ms main-
		 * loop watcher then (re)sizes reac-capture / reac-playback to its widths. No
		 * box node exists until then (nothing plugged = nothing in the graph). */
		adc.src = &src;
		adc.sink = sink;
		adc.scfg = src_cfg;
		/* #208: let the sink's badge timer keep the reac-capture node's link-state /
		 * box-model / box-width in sync (it has no pacer handle of its own). Same source
		 * slot the autodetect watcher rebuilds, so a live box-width change is followed. */
		reac_sink_node_set_peer_source(sink, &src);
		ad_timer = pw_loop_add_timer(loop, on_autodetect_timer, &adc);
		if (ad_timer) {
			struct timespec first = { 0, 200 * 1000000L };
			struct timespec interval = { 0, 200 * 1000000L };
			pw_loop_update_timer(loop, ad_timer, &first, &interval, false);
		}
		fprintf(stderr, "reac-pw: MASTER autodetect — reac-capture / reac-playback "
		        "appear sized to the box once it is recognized on the wire\n");
	} else {
		/* No recognizer (slave, or pcap / no-TX master): expose the source now. Slave
		 * -> the 40-ch downstream; master -> the --box width (0 -> the 40-ch fabric). */
		int src_ch = (role == REAC_ROLE_MASTER) ? master_box_in : 0;
		if (reac_source_node_ensure(&src, &src_cfg, src_ch, box_label) != 0) {
			fprintf(stderr, "reac-pw: failed to create reac:capture node\n");
			return 1;
		}
	}

	if (reac_rx_start(&rx) != 0) {
		fprintf(stderr, "reac-pw: failed to start RX feeder\n");
		return 1;
	}

	pw_main_loop_run(g_loop);

	reac_rx_stop(&rx);
	if (ad_timer)
		pw_loop_destroy_source(loop, ad_timer);   /* stop the autodetect watcher first */
	reac_source_node_destroy(src);                /* may be NULL (never recognized) */
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
