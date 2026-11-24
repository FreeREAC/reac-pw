// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_source_node -- reac-pw's OWN capture-node code, with no PipeWire server and no wire.
 *
 * Same method as tests/test_reac_sink_node_own.c: #include the .c to reach what is not
 * exported, build a struct reac_source_node by hand, give it a stand-in stream, and define
 * the libpipewire calls the code makes on it (update_properties, disconnect, connect)
 * here, where the executable's definition wins over the shared library's. What is pinned
 * is what the capture node says to the graph: the badge it mirrors from the playback
 * side, the slave segment's whole answer, the door's keys going on and coming off again,
 * the rate renegotiation, and the role re-open the main loop collects. */
#include "../src/reac_source_node.c"

#include <stdio.h>
#include <string.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

#define MAXREC 64
static int g_updates;
static int g_disconnects, g_connects;
static int g_connect_rc;               /* what the next pw_stream_connect returns */
static char g_key[MAXREC][64];
static char g_val[MAXREC][128];
static int g_n;

static void rec_reset(void)
{
	g_updates = g_disconnects = g_connects = g_n = 0;
	g_connect_rc = 0;
}

static const char *rec_get(const char *key)
{
	for (int i = 0; i < g_n; i++)
		if (!strcmp(g_key[i], key))
			return g_val[i];
	return NULL;
}

int pw_stream_update_properties(struct pw_stream *stream, const struct spa_dict *dict)
{
	(void)stream;
	g_updates++;
	const struct spa_dict_item *it;
	spa_dict_for_each(it, dict) {
		int i;
		for (i = 0; i < g_n; i++)
			if (!strcmp(g_key[i], it->key))
				break;
		if (i == g_n) {
			if (g_n == MAXREC)
				return 0;
			g_n++;
		}
		snprintf(g_key[i], sizeof g_key[i], "%s", it->key);
		snprintf(g_val[i], sizeof g_val[i], "%s", it->value ? it->value : "");
	}
	return 0;
}

int pw_stream_disconnect(struct pw_stream *stream)
{
	(void)stream;
	g_disconnects++;
	return 0;
}

int pw_stream_connect(struct pw_stream *stream, enum pw_direction direction, uint32_t target_id,
                      enum pw_stream_flags flags, const struct spa_pod **params,
                      uint32_t n_params)
{
	(void)stream; (void)direction; (void)target_id; (void)flags; (void)params; (void)n_params;
	g_connects++;
	return g_connect_rc;
}

#define FAKE_STREAM ((struct pw_stream *)(uintptr_t)0x1000)

static struct reac_source_node *new_node(int with_stream)
{
	struct reac_source_node *n = calloc(1, sizeof *n);
	if (n) {
		n->channels = 8;
		n->sample_rate = 48000;
		n->stream = with_stream ? FAKE_STREAM : NULL;
	}
	return n;
}

/* ---- the badge mirrored from the playback side --------------------------------- */
static int test_publish_link(void)
{
	struct reac_source_node *n = new_node(0);
	CHK(n);
	struct reac_identity id;
	memset(&id, 0, sizeof id);
	const struct reac_box_model *bm = reac_box_catalogue_by_token("s4000s");
	CHK(bm);

	/* A node with no stream is not stamped, and says so (the sink re-stamps a blank one). */
	rec_reset();
	reac_source_node_publish_link(n, "established", bm, 0x0050c2112233ull, &id);
	CHK(g_updates == 0 && !reac_source_node_badge_stamped(n));
	reac_source_node_publish_link(NULL, "established", bm, 0, &id);   /* NULL node is safe */
	CHK(!reac_source_node_badge_stamped(NULL));

	n->stream = FAKE_STREAM;
	reac_source_node_publish_link(n, "established", bm, 0x0050c2112233ull, &id);
	CHK(g_updates == 1 && reac_source_node_badge_stamped(n));
	CHK(!strcmp(rec_get(REAC_PROP_LINK_STATE), "established"));
	CHK(!strcmp(rec_get(REAC_PROP_BOX_MODEL), bm->token));
	CHK(!strcmp(rec_get(REAC_PROP_BOX_WIDTH), "32x8"));
	CHK(!strcmp(rec_get(REAC_PROP_BOX_MAC), "00:50:c2:11:22:33"));

	/* A box with no model and no address stamps "none" / "0x0" / the no-address word, so a
	 * departed box's values are overwritten, never left. */
	reac_source_node_publish_link(n, "probing", NULL, 0, NULL);
	CHK(!strcmp(rec_get(REAC_PROP_BOX_MODEL), "none"));
	CHK(!strcmp(rec_get(REAC_PROP_BOX_WIDTH), "0x0"));
	CHK(!strcmp(rec_get(REAC_PROP_BOX_MAC), REAC_BOX_MAC_NONE));
	free(n);
	return 0;
}

/* The receive-only join: the box is the MASTER, and this node alone carries what it is. */
static int test_publish_box_master(void)
{
	struct reac_source_node *n = new_node(1);
	CHK(n);
	rec_reset();
	reac_source_node_publish_box_master(n, 16, 0x0050c2aabbccull, 1);
	CHK(g_updates == 1);
	CHK(!strcmp(rec_get(REAC_PROP_LINK_STATE), "established"));
	CHK(!strcmp(rec_get(REAC_PROP_BOX_WIDTH), "16"));
	CHK(!strcmp(rec_get(REAC_PROP_BOX_MAC), "00:50:c2:aa:bb:cc"));
	reac_source_node_publish_box_master(n, 0, 0, 0);          /* unlocked, width unknown */
	CHK(!strcmp(rec_get(REAC_PROP_LINK_STATE), "probing"));
	CHK(!strcmp(rec_get(REAC_PROP_BOX_WIDTH), "16"));          /* a width of 0 is not stamped */
	free(n);
	return 0;
}

/* ---- the slave segment's whole answer, in one update ------------------------- */
static int test_publish_segment(void)
{
	struct reac_source_node *n = new_node(1);
	CHK(n);
	rec_reset();
	reac_source_node_publish_segment(n, "1", "applied", "none", NULL);
	CHK(g_updates == 1);
	CHK(!strcmp(rec_get(REAC_PROP_ROLE), "1"));
	CHK(!strcmp(rec_get(REAC_PROP_ROLE_STATE), "applied"));
	CHK(!strcmp(rec_get(REAC_PROP_ROLE_REFUSED), "none"));
	CHK(rec_get(REAC_PROP_MASTER_STATE) == NULL);              /* no answer, no key: absence says so */

	/* A NULL part is left out, not stamped empty. */
	rec_reset();
	reac_source_node_publish_segment(n, NULL, "applied", NULL, NULL);
	CHK(g_updates == 1 && rec_get(REAC_PROP_ROLE) == NULL && rec_get(REAC_PROP_ROLE_REFUSED) == NULL);

	struct reac_segment_answer a;
	memset(&a, 0, sizeof a);
	snprintf(a.master_state, sizeof a.master_state, "%s", "foreign");
	snprintf(a.master_mac, sizeof a.master_mac, "%s", "aa:bb:cc:dd:ee:ff");
	snprintf(a.pace_source, sizeof a.pace_source, "%s", "foreign-master");
	snprintf(a.conflict, sizeof a.conflict, "%s", "0");
	snprintf(a.rival_kind, sizeof a.rival_kind, "%s", "none");
	snprintf(a.refusal, sizeof a.refusal, "%s", "none");
	snprintf(a.rate, sizeof a.rate, "%s", "48000");
	rec_reset();
	reac_source_node_publish_segment(n, "1", "applied", "none", &a);
	CHK(g_updates == 1);                                       /* ONE update, not two */
	CHK(!strcmp(rec_get(REAC_PROP_MASTER_STATE), "foreign"));
	CHK(!strcmp(rec_get(REAC_PROP_MASTER_MAC), "aa:bb:cc:dd:ee:ff"));
	CHK(!strcmp(rec_get(REAC_PROP_PACE_SOURCE), "foreign-master"));
	CHK(!strcmp(rec_get(REAC_PROP_MASTER_CONFLICT), "0"));
	CHK(!strcmp(rec_get(REAC_PROP_RIVAL_KIND), "none"));
	CHK(!strcmp(rec_get(REAC_PROP_REFUSAL), "none"));
	CHK(!strcmp(rec_get(REAC_PROP_RATE), "48000"));

	n->stream = NULL;                                          /* no stream: nothing to say it to */
	rec_reset();
	reac_source_node_publish_segment(n, "1", "applied", "none", &a);
	CHK(g_updates == 0);
	free(n);
	return 0;
}

/* ---- a master's door when its box has no outputs --------------------------------- */
static void door_cb(void *ctx, const struct spa_pod *props)
{
	(void)props;
	(*(int *)ctx)++;
}

static int test_door_keys_go_when_the_door_is_unbound(void)
{
	struct reac_source_node *n = new_node(1);
	CHK(n);
	int calls = 0;
	CHK(!reac_source_node_door_bound(n));
	reac_source_node_set_door(n, door_cb, &calls);
	CHK(reac_source_node_door_bound(n));

	/* What the door stamps is remembered, key by key... */
	rec_reset();
	struct spa_dict_item items[2] = { SPA_DICT_ITEM_INIT("reac.rate", "48000"),
	                                  SPA_DICT_ITEM_INIT("reac.cfg.role.state", "applied") };
	struct spa_dict d = SPA_DICT_INIT(items, 2);
	reac_source_node_update_props(n, &d);
	CHK(g_updates == 1 && !strcmp(rec_get("reac.rate"), "48000"));
	CHK(n->door_keys && n->door_keys->dict.n_items == 2);

	/* ...and UNBOUND the door's keys are each EMPTIED (a property update merges on the
	 * server, so a key merely not re-sent would keep its old value there), then forgotten. */
	rec_reset();
	reac_source_node_set_door(n, NULL, NULL);
	CHK(!reac_source_node_door_bound(n));
	CHK(g_updates == 1);
	CHK(rec_get("reac.rate") && !strcmp(rec_get("reac.rate"), ""));
	CHK(rec_get("reac.cfg.role.state") && !strcmp(rec_get("reac.cfg.role.state"), ""));
	CHK(n->door_keys == NULL);

	/* Unbinding a door that stamped nothing says nothing. */
	rec_reset();
	reac_source_node_set_door(n, door_cb, &calls);
	reac_source_node_set_door(n, NULL, NULL);
	CHK(g_updates == 0);

	/* No stream, no door stamp, and nothing remembered. */
	n->stream = NULL;
	reac_source_node_update_props(n, &d);
	CHK(g_updates == 0 && n->door_keys == NULL);
	free(n);
	return 0;
}

/* ---- the role re-open the main loop collects ---------------------------------- */
static int test_take_reopen_role(void)
{
	struct reac_source_node *n = new_node(0);
	CHK(n);
	CHK(reac_source_node_take_reopen_role(n) == -1);          /* nothing pending */
	CHK(reac_source_node_take_reopen_role(NULL) == -1);
	atomic_store(&n->reopen_role, 0 + 1);                      /* role 0 is stored as 1 */
	CHK(reac_source_node_take_reopen_role(n) == 0);
	CHK(reac_source_node_take_reopen_role(n) == -1);          /* read AND cleared */
	atomic_store(&n->reopen_role, 1 + 1);
	CHK(reac_source_node_take_reopen_role(n) == 1);
	free(n);
	return 0;
}

/* ---- the rate: renegotiate the format only when the wire's rate moved ------------- */
static int test_publish_rate(void)
{
	struct reac_source_node *n = new_node(1);
	CHK(n);
	rec_reset();
	reac_source_node_publish_rate(n, 48000);      /* already presenting it */
	reac_source_node_publish_rate(n, 0);          /* never a real accepted rate */
	reac_source_node_publish_rate(n, -1);
	CHK(g_disconnects == 0 && g_connects == 0 && n->sample_rate == 48000);

	reac_source_node_publish_rate(n, 96000);      /* the wire moved: disconnect + connect */
	CHK(g_disconnects == 1 && g_connects == 1);
	CHK(n->sample_rate == 96000);
	CHK(atomic_load(&n->rate_reconnecting) == 0);  /* the RT guard is lowered again */

	reac_source_node_publish_rate(n, 96000);      /* caught up: no churn */
	CHK(g_disconnects == 1 && g_connects == 1);

	/* A failed reconnect: the OLD rate is re-asserted so the node is not left
	 * disconnected, and the node's rate stays what it really presents. */
	g_connect_rc = -1;
	reac_source_node_publish_rate(n, 44100);
	CHK(g_disconnects == 2 && g_connects == 3);   /* the attempt, then the fallback */
	CHK(n->sample_rate == 96000);
	CHK(atomic_load(&n->rate_reconnecting) == 0);

	n->stream = NULL;                              /* no stream: nothing to renegotiate */
	g_connect_rc = 0;
	reac_source_node_publish_rate(n, 48000);
	CHK(g_disconnects == 2 && n->sample_rate == 96000);
	free(n);
	return 0;
}

/* ---- the RT callback's one diagnostic line, printed by the main loop ----------------- */
static int test_drain_log(void)
{
	struct reac_source_node *n = new_node(0);
	CHK(n);
	FILE *out = tmpfile();
	CHK(out);
	char buf[64] = "";
	snprintf(n->log_line, sizeof n->log_line, "%s", "ring peak 12\n");

	reac_source_node_drain_log(n, out);            /* not pending: nothing is written */
	rewind(out);
	CHK(fread(buf, 1, sizeof buf - 1, out) == 0);

	atomic_store(&n->log_pending, 1);
	reac_source_node_drain_log(n, out);
	CHK(atomic_load(&n->log_pending) == 0);        /* the handshake: main clears it */
	rewind(out);
	size_t got = fread(buf, 1, sizeof buf - 1, out);
	buf[got] = '\0';
	CHK(!strcmp(buf, "ring peak 12\n"));

	reac_source_node_drain_log(n, out);            /* once only */
	reac_source_node_drain_log(NULL, out);
	reac_source_node_drain_log(n, NULL);
	fclose(out);
	free(n);
	return 0;
}

/* ---- ensure: the same box is a no-op, never a rebuild -------------------------------- */
static int test_ensure_same_box_is_a_no_op(void)
{
	struct reac_source_node_cfg cfg;
	memset(&cfg, 0, sizeof cfg);
	CHK(reac_source_node_ensure(NULL, &cfg, 8, "x") == -1);
	struct reac_source_node *slot = NULL;
	CHK(reac_source_node_ensure(&slot, NULL, 8, "x") == -1);

	struct reac_source_node *n = new_node(1);
	CHK(n);
	snprintf(n->label, sizeof n->label, "%s", "Stage left");
	slot = n;
	/* Same width, same label: the live node stays. A width past 40 settles on 40, so the
	 * comparison is against what _new would build, not the raw request. */
	CHK(reac_source_node_ensure(&slot, &cfg, 8, "Stage left") == 0 && slot == n);
	n->channels = REAC_MAX_CHANNELS;
	CHK(reac_source_node_ensure(&slot, &cfg, 0, "Stage left") == 0 && slot == n);
	CHK(reac_source_node_ensure(&slot, &cfg, 99, "Stage left") == 0 && slot == n);
	free(n);
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= test_publish_link();
	rc |= test_publish_box_master();
	rc |= test_publish_segment();
	rc |= test_door_keys_go_when_the_door_is_unbound();
	rc |= test_take_reopen_role();
	rc |= test_publish_rate();
	rc |= test_drain_log();
	rc |= test_ensure_same_box_is_a_no_op();
	if (rc == 0)
		printf("test_reac_source_node_own: ok\n");
	return rc;
}
