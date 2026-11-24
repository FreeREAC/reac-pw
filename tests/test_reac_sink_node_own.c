// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_sink_node -- reac-pw's OWN sink code, with no PipeWire server and no wire.
 *
 * This file #includes src/reac_sink_node.c so it reaches the statics, builds a
 * struct reac_sink_node by hand (zeroed, pacer fields scripted the way the pacer
 * thread would write them) and gives it a stand-in `stream`. The two libpipewire
 * calls the code under test makes on it, pw_stream_update_properties and
 * pw_stream_update_params, are defined HERE: the executable's definition wins over
 * the shared library's, so the call lands in a recorder instead of a server.
 *
 * What is pinned is what reaches the node's properties, which is the contract the
 * netns scripts used to reach only through a running daemon: the badge a granted box
 * gets (reac.link-state, reac.box.mac, reac.box-model, reac.box-width), and that an
 * unchanged state is NOT re-published. */
#include "../src/reac_sink_node.c"

#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

/* ---- the recorder behind the stand-in stream -------------------------------- */
#define MAXREC 64
static int g_updates;                  /* pw_stream_update_properties calls */
static int g_param_updates;            /* pw_stream_update_params calls */
static char g_last_key[MAXREC][64];    /* the merged view: key -> last value seen */
static char g_last_val[MAXREC][128];
static int g_n_last;

static void rec_reset(void)
{
	g_updates = g_param_updates = g_n_last = 0;
}

static const char *rec_get(const char *key)
{
	for (int i = 0; i < g_n_last; i++)
		if (!strcmp(g_last_key[i], key))
			return g_last_val[i];
	return NULL;
}

int pw_stream_update_properties(struct pw_stream *stream, const struct spa_dict *dict)
{
	(void)stream;
	g_updates++;
	const struct spa_dict_item *it;
	spa_dict_for_each(it, dict) {
		int i;
		for (i = 0; i < g_n_last; i++)
			if (!strcmp(g_last_key[i], it->key))
				break;
		if (i == g_n_last) {
			if (g_n_last == MAXREC)
				return 0;
			g_n_last++;
		}
		snprintf(g_last_key[i], sizeof g_last_key[i], "%s", it->key);
		snprintf(g_last_val[i], sizeof g_last_val[i], "%s", it->value ? it->value : "");
	}
	return 0;
}

int pw_stream_update_params(struct pw_stream *stream, const struct spa_pod **params,
                            uint32_t n_params)
{
	(void)stream; (void)params; (void)n_params;
	g_param_updates++;
	return 0;
}

#define FAKE_STREAM ((struct pw_stream *)(uintptr_t)0x1000)

static struct reac_sink_node *new_node(int channels)
{
	struct reac_sink_node *n = calloc(1, sizeof *n);
	if (!n)
		return NULL;
	n->channels = channels;
	reac_box_rows_init(&n->box_rows);
	for (int c = 0; c < REAC_MAX_CHANNELS; c++) {
		n->chan_vol[c] = 1.0f;
		atomic_store(&n->chan_target[c], 1.0f);
	}
	return n;
}

/* ---- sink_build_desc: the one formatter, at build and at relabel ------------ */
static int test_build_desc(void)
{
	char d[128];
	sink_build_desc(d, sizeof d, "Stage left", 8);
	CHK(!strcmp(d, "Stage left \xe2\x80\x94 8 ch (REAC box outputs)"));
	sink_build_desc(d, sizeof d, NULL, 16);
	CHK(!strcmp(d, "REAC 16ch playback (downstream master TX)"));
	sink_build_desc(d, sizeof d, "", 16);                 /* empty label is no label */
	CHK(!strcmp(d, "REAC 16ch playback (downstream master TX)"));
	char tiny[10];
	sink_build_desc(tiny, sizeof tiny, "Stage left", 8);   /* bounded, always terminated */
	CHK(strlen(tiny) == sizeof tiny - 1);
	return 0;
}

/* ---- sink_publish: the gain the RT thread reads, mute folded in ------------- */
static int test_publish_gain(void)
{
	struct reac_sink_node *n = new_node(4);
	CHK(n);
	n->chan_vol[0] = 0.5f; n->chan_vol[1] = 0.25f; n->chan_vol[2] = 1.0f; n->chan_vol[3] = 0.0f;
	rec_reset();
	sink_publish(n);                    /* no stream: shadows go to the RT side, no param push */
	CHK(atomic_load(&n->chan_target[0]) == 0.5f && atomic_load(&n->chan_target[1]) == 0.25f);
	CHK(atomic_load(&n->chan_target[2]) == 1.0f && atomic_load(&n->chan_target[3]) == 0.0f);
	CHK(g_param_updates == 0);

	n->muted = true;                    /* mute is folded into the target, the shadow stays */
	sink_publish(n);
	for (int c = 0; c < 4; c++)
		CHK(atomic_load(&n->chan_target[c]) == 0.0f);
	CHK(n->chan_vol[0] == 0.5f);
	n->muted = false;
	sink_publish(n);
	CHK(atomic_load(&n->chan_target[0]) == 0.5f);

	/* A channel past n->channels is never written. */
	atomic_store(&n->chan_target[7], 0.75f);
	sink_publish(n);
	CHK(atomic_load(&n->chan_target[7]) == 0.75f);

	n->stream = FAKE_STREAM;            /* a live stream gets the Props re-advertised */
	sink_publish(n);
	CHK(g_param_updates == 1);
	free(n);
	return 0;
}

/* ---- the badge a granted box gets -------------------------------------------- *
 * The pacer thread writes these fields; the main loop reads them with atomic loads and
 * publishes. Scripting them is scripting the pacer's side of the seam. */
static void script_box(struct reac_sink_node *n, enum reac_master_state st, int in_ch,
                       int out_ch, uint64_t mac48)
{
	struct reac_identity id;
	memset(&id, 0, sizeof id);
	id.has_reac_version = 1;      /* the identity page is in: the name does not wait */
	n->pacer.rx_identity = id;
	atomic_store(&n->pacer.fsm_state, (int)st);
	atomic_store(&n->pacer.declared_in, in_ch);
	atomic_store(&n->pacer.declared_out, out_ch);
	atomic_store(&n->pacer.recognized_box_mac, mac48);
}

static int test_badge_of_a_granted_box(void)
{
	struct reac_sink_node *n = new_node(8);
	CHK(n);
	n->stream = FAKE_STREAM;
	rec_reset();
	script_box(n, REAC_M_ESTABLISHED, 32, 8, 0x0050c2112233ull);
	sink_publish_link_props(n);
	CHK(g_updates == 1);
	CHK(rec_get(REAC_PROP_LINK_STATE) && !strcmp(rec_get(REAC_PROP_LINK_STATE), "established"));
	CHK(rec_get(REAC_PROP_BOX_WIDTH) && !strcmp(rec_get(REAC_PROP_BOX_WIDTH), "32x8"));
	CHK(rec_get(REAC_PROP_BOX_MAC) && !strcmp(rec_get(REAC_PROP_BOX_MAC), "00:50:c2:11:22:33"));
	CHK(rec_get(REAC_PROP_BOX_MODEL) && strcmp(rec_get(REAC_PROP_BOX_MODEL), "none") != 0);
	CHK(rec_get(REAC_PROP_BOX_SOURCE) && !strcmp(rec_get(REAC_PROP_BOX_SOURCE), "wire"));
	CHK(rec_get(REAC_PROP_HEADAMP_CHANNELS) && !strcmp(rec_get(REAC_PROP_HEADAMP_CHANNELS), "32"));
	/* The shadows the change guard reads follow what was published. */
	CHK(n->link_state_last == REAC_LINK_ESTABLISHED && n->box_mac_last == 0x0050c2112233ull);
	CHK(n->box_model_last && n->box_model_last->in_ch == 32 && n->box_model_last->out_ch == 8);

	/* NOTHING CHANGED: nothing is published (no spam of pw_stream_update_properties). */
	sink_publish_link_props(n);
	sink_publish_link_props(n);
	CHK(g_updates == 1);

	/* A different box MAC alone is a change. */
	atomic_store(&n->pacer.recognized_box_mac, 0x0050c2445566ull);
	sink_publish_link_props(n);
	CHK(g_updates == 2);
	CHK(!strcmp(rec_get(REAC_PROP_BOX_MAC), "00:50:c2:44:55:66"));

	/* A drop since the last look reads DROPPED once, then the state is read again. */
	atomic_fetch_add(&n->pacer.drops[0], 1);
	sink_publish_link_props(n);
	CHK(g_updates == 3 && !strcmp(rec_get(REAC_PROP_LINK_STATE), "dropped"));
	sink_publish_link_props(n);
	CHK(g_updates == 4 && !strcmp(rec_get(REAC_PROP_LINK_STATE), "established"));

	/* THE BOX LEAVES: every key is stamped EVEN WHEN EMPTY so a departed box's values do
	 * not stay behind on the node (update_properties merges). */
	script_box(n, REAC_M_PROBING, 0, 0, 0);
	sink_publish_link_props(n);
	CHK(g_updates == 5);
	CHK(!strcmp(rec_get(REAC_PROP_LINK_STATE), "probing"));
	CHK(!strcmp(rec_get(REAC_PROP_BOX_MODEL), "none"));
	CHK(!strcmp(rec_get(REAC_PROP_BOX_WIDTH), "0x0"));
	CHK(!strcmp(rec_get(REAC_PROP_BOX_MAC), REAC_BOX_MAC_NONE));   /* an address too is cleared */
	CHK(!strcmp(rec_get(REAC_PROP_BOX_SOURCE), "none"));
	CHK(!strcmp(rec_get(REAC_PROP_HEADAMP_CHANNELS), "0"));
	free(n);
	return 0;
}

/* Without a stream the shadows are still kept (the peer capture is still stamped), and
 * nothing is sent to a node that is not there. */
static int test_no_stream_keeps_shadows_and_sends_nothing(void)
{
	struct reac_sink_node *n = new_node(8);
	CHK(n);
	rec_reset();
	script_box(n, REAC_M_ESTABLISHED, 32, 8, 0x0050c2112233ull);
	sink_publish_link_props(n);
	CHK(g_updates == 0);
	CHK(n->link_state_last == REAC_LINK_ESTABLISHED && n->box_model_last);
	free(n);
	return 0;
}

/* The identity page that has not come in yet holds the NAME for REACPW_BOX_NAME_WAIT_NS,
 * because a relabel rebuilds the nodes: no row, then the row once the page is in. */
static int test_the_name_waits_for_the_identity_page(void)
{
	struct reac_sink_node *n = new_node(8);
	CHK(n);
	n->stream = FAKE_STREAM;
	rec_reset();
	script_box(n, REAC_M_ESTABLISHED, 32, 8, 0x0050c2112233ull);
	n->pacer.rx_identity.has_reac_version = 0;          /* the page has not answered */
	sink_publish_link_props(n);
	CHK(!strcmp(rec_get(REAC_PROP_BOX_MODEL), "none"));
	CHK(!strcmp(rec_get(REAC_PROP_BOX_WIDTH), "0x0"));
	n->pacer.rx_identity.has_reac_version = 1;          /* it arrives */
	sink_publish_link_props(n);
	CHK(strcmp(rec_get(REAC_PROP_BOX_MODEL), "none") != 0);
	CHK(!strcmp(rec_get(REAC_PROP_BOX_WIDTH), "32x8"));
	free(n);
	return 0;
}

/* ---- the head-amp capability, as this node sees the segment ------------------ *
 * One function read by the write door AND the publisher, so a refusal and the property
 * that answers with it cannot disagree. */
static int test_headamp_capability(void)
{
	struct reac_sink_node *n = new_node(8);
	CHK(n);
	/* A box mastering the segment: this node carries the upstream ring. It outranks no-box. */
	struct reac_ring *ring = (struct reac_ring *)(uintptr_t)0x2000;
	n->upstream_ring = ring;
	CHK(sink_headamp_capability(n) == REAC_HEADAMP_REFUSE_BOX_MASTER);
	n->upstream_ring = NULL;

	/* A pacer that never opened has no recogniser: no preamps, never a zeroed atomic read as
	 * a measurement. */
	script_box(n, REAC_M_ESTABLISHED, 32, 8, 0x0050c2112233ull);
	atomic_store(&n->pacer.recognized_headamp_base, 0);
	n->pacer_open = 0;
	CHK(sink_headamp_capability(n) == REAC_HEADAMP_REFUSE_NO_BOX);

	/* Opened, a box declared, and the chassis strap not announced (-1): no wire address. */
	n->pacer_open = 1;
	script_box(n, REAC_M_ESTABLISHED, 32, 8, 0x0050c2112233ull);
	atomic_store(&n->pacer.recognized_headamp_base, -1);
	CHK(sink_headamp_capability(n) == REAC_HEADAMP_REFUSE_NO_BASE);

	/* The strap announced: the segment can carry a head-amp record. */
	atomic_store(&n->pacer.recognized_headamp_base, 0);
	CHK(sink_headamp_capability(n) == REAC_HEADAMP_REFUSE_NONE);

	/* Opened but no box declared yet: the width is 0, so no box. */
	script_box(n, REAC_M_PROBING, 0, 0, 0);
	CHK(sink_headamp_capability(n) == REAC_HEADAMP_REFUSE_NO_BOX);
	free(n);
	return 0;
}

/* ---- the rate answer: reac.rate and its source/state ------------------------- */
static int test_rate_props(void)
{
	struct reac_sink_node *n = new_node(8);
	CHK(n);
	n->stream = FAKE_STREAM;
	n->sample_rate = 48000;           /* the node already presents the wire's rate: no reconnect */
	n->pacer.drivable_mask = REAC_RATE_ALL_BITS;
	atomic_store(&n->pacer.rate_hz, 48000);
	atomic_store(&n->pacer.rate_asserted, 0);
	atomic_store(&n->pacer.rate_reestablishing, 0);
	atomic_store(&n->pacer.rate_refused, (int)REAC_RATE_REFUSE_NONE);
	rec_reset();
	sink_publish_rate_props(n);
	CHK(g_updates == 1);
	CHK(!strcmp(rec_get(REAC_PROP_RATE), "48000"));
	CHK(!strcmp(rec_get(REAC_PROP_RATE_SOURCE), REAC_RATE_SOURCE_CONVENTION));
	CHK(!strcmp(rec_get(REAC_PROP_RATE_STATE), REAC_RATE_STATE_APPLIED));

	sink_publish_rate_props(n);       /* nothing moved: nothing published */
	CHK(g_updates == 1);

	atomic_store(&n->pacer.rate_asserted, 1);        /* an operator's own write stands */
	sink_publish_rate_props(n);
	CHK(g_updates == 2 && !strcmp(rec_get(REAC_PROP_RATE_SOURCE), REAC_RATE_SOURCE_ASSERTED));

	atomic_store(&n->pacer.rate_reestablishing, 1);  /* a change is owed, not yet done */
	sink_publish_rate_props(n);
	CHK(g_updates == 3 && !strcmp(rec_get(REAC_PROP_RATE_STATE), REAC_RATE_STATE_PENDING));

	/* No door (no stream, no capture peer): the node says nothing about rate at all, and
	 * does not move its shadow, so the answer is owed in full when a door appears. */
	n->stream = NULL;
	atomic_store(&n->pacer.rate_reestablishing, 0);
	sink_publish_rate_props(n);
	CHK(g_updates == 3);
	CHK(n->rate_reestablishing_last == 1);
	free(n);
	return 0;
}

/* ---- the role answer ----------------------------------------------------------- *
 * This node exists only in the master role, so reac.role is the FACT it is running
 * (master) whatever reac.cfg.role asked; the state beside it is what moves. */
static int test_role_props(void)
{
	struct reac_sink_node *n = new_node(8);
	CHK(n);
	n->stream = FAKE_STREAM;
	n->role_state = REAC_ROLE_STATE_APPLIED;
	n->role_refused = REAC_ROLE_REFUSE_NONE;
	rec_reset();
	sink_publish_role_props(n);
	CHK(g_updates == 1);
	CHK(!strcmp(rec_get(REAC_PROP_ROLE), "0"));      /* REAC_CFG_ROLE_VALUE_MASTER */
	CHK(!strcmp(rec_get(REAC_PROP_ROLE_STATE), REAC_ROLE_STATE_APPLIED));

	sink_publish_role_props(n);
	CHK(g_updates == 1);                              /* unchanged */

	n->role_state = REAC_ROLE_STATE_REESTABLISH_PENDING;
	sink_publish_role_props(n);
	CHK(g_updates == 2);
	CHK(!strcmp(rec_get(REAC_PROP_ROLE_STATE), REAC_ROLE_STATE_REESTABLISH_PENDING));
	CHK(!strcmp(rec_get(REAC_PROP_ROLE), "0"));      /* still master: never the request */

	n->role_refused = REAC_ROLE_REFUSE_MALFORMED;    /* a refusal alone is a change */
	sink_publish_role_props(n);
	CHK(g_updates == 3);
	CHK(!strcmp(rec_get(REAC_PROP_ROLE_REFUSED), reac_role_refuse_code(REAC_ROLE_REFUSE_MALFORMED)));

	/* No door: nothing is said and the shadow stays, so the answer is owed later. */
	n->stream = NULL;
	n->role_state = REAC_ROLE_STATE_APPLIED;
	sink_publish_role_props(n);
	CHK(g_updates == 3 && n->role_state_last == REAC_ROLE_STATE_REESTABLISH_PENDING);
	free(n);
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= test_build_desc();
	rc |= test_publish_gain();
	rc |= test_badge_of_a_granted_box();
	rc |= test_no_stream_keeps_shadows_and_sends_nothing();
	rc |= test_the_name_waits_for_the_identity_page();
	rc |= test_headamp_capability();
	rc |= test_rate_props();
	rc |= test_role_props();
	if (rc == 0)
		printf("test_reac_sink_node_own: ok\n");
	return rc;
}
