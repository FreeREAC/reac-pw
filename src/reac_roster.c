// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_roster — see reac_roster.h for what this publishes and why it is a delta.

#include "reac_roster.h"

#include <stdio.h>
#include <string.h>

const char *reac_roster_state_name(enum reac_roster_state s)
{
	switch (s) {
	case REAC_ROSTER_PROBING:     return "probing";
	case REAC_ROSTER_ESTABLISHED: return "established";
	case REAC_ROSTER_SLAVE:       return "slave";
	case REAC_ROSTER_TAP:         return "tap";
	case REAC_ROSTER_REFUSED:     return "refused";
	case REAC_ROSTER_IGNORED:     return "ignored";
	}
	/* No default: the compiler's exhaustiveness proof is the point, and a state added
	 * without a word here must not compile into a silent "". */
	return "probing";
}

void reac_roster_init(struct reac_roster *r)
{
	memset(r, 0, sizeof *r);
}

void reac_roster_begin(struct reac_roster *r)
{
	r->n_want = 0;
	r->overflow = 0;
}

int reac_roster_add(struct reac_roster *r, const char *name, enum reac_roster_state state,
                    const char *model, const char *role, const char *source, int in, int out)
{
	if (!r || !name || !*name)
		return -1;
	if (r->n_want >= REAC_ROSTER_MAX) {
		r->overflow++;
		return -1;
	}
	/* BYTE ORDER, INSERTED. The list is at most 32 long and grows once per tick, so the
	 * insertion is cheaper than the sort and keeps the invariant true at every step. */
	int at = 0;
	while (at < r->n_want) {
		int c = strcmp(r->want[at].name, name);
		if (c == 0)
			return -1;   /* two tables claiming one wire: the caller's bug, said out loud */
		if (c > 0)
			break;
		at++;
	}
	for (int i = r->n_want; i > at; i--)
		r->want[i] = r->want[i - 1];
	struct reac_roster_seg *s = &r->want[at];
	memset(s, 0, sizeof *s);
	snprintf(s->name, sizeof s->name, "%s", name);
	s->state = state;
	/* `none` IS THE ANSWER, not an empty string. An absent box must SAY absent: the
	 * console rendered `none / 0 in` for a whole segment once, and the fix was to remove
	 * the segment's node — not to make its absence unreadable. */
	snprintf(s->model, sizeof s->model, "%s", model && *model ? model : "none");
	snprintf(s->role, sizeof s->role, "%s", role && *role ? role : "auto");
	snprintf(s->source, sizeof s->source, "%s", source && *source ? source : "autodetected");
	s->in = in;
	s->out = out;
	r->n_want++;
	return 0;
}

/* One field of one segment, as the string the property carries. Field order IS key order,
 * and both are the spec's. */
static void field_val(const struct reac_roster_seg *s, int f, char *out, size_t len)
{
	switch (f) {
	case 0: snprintf(out, len, "%s", s->name); return;
	case 1: snprintf(out, len, "%s", reac_roster_state_name(s->state)); return;
	case 2: snprintf(out, len, "%s", s->model); return;
	case 3: snprintf(out, len, "%s", s->role); return;
	case 4: snprintf(out, len, "%s", s->source); return;
	default: snprintf(out, len, "%d/%d", s->in, s->out); return;
	}
}

static const char *field_key(int f)
{
	static const char *k[REAC_ROSTER_FIELDS] = { "name", "state", "model", "role",
	                                             "source", "width" };
	return k[f];
}

struct emit_ctx {
	struct reac_roster_kv *out;
	int max;
	int n;
	int over;   /* the delta does not fit: REFUSE, never truncate */
};

static void emit(struct emit_ctx *e, const char *key, const char *val, int remove)
{
	if (e->n >= e->max) {
		e->over = 1;
		e->n++;
		return;
	}
	struct reac_roster_kv *kv = &e->out[e->n++];
	snprintf(kv->key, sizeof kv->key, "%s", key);
	snprintf(kv->val, sizeof kv->val, "%s", val ? val : "");
	kv->remove = remove;
}

int reac_roster_delta(struct reac_roster *r, struct reac_roster_kv *out, int max)
{
	if (!r || !out || max < 0)
		return -1;
	struct emit_ctx e = { out, max, 0, 0 };

	if (r->n_want != r->n_pub) {
		char n[16];
		snprintf(n, sizeof n, "%d", r->n_want);
		emit(&e, "reac.roster.n", n, 0);
	}
	for (int i = 0; i < r->n_want; i++) {
		const struct reac_roster_seg *w = &r->want[i];
		const struct reac_roster_seg *p = i < r->n_pub ? &r->pub[i] : NULL;
		for (int f = 0; f < REAC_ROSTER_FIELDS; f++) {
			char wv[112], pv[112];
			field_val(w, f, wv, sizeof wv);
			if (p) {
				field_val(p, f, pv, sizeof pv);
				if (strcmp(wv, pv) == 0)
					continue;   /* unchanged: the whole point of this module */
			}
			char key[40];
			snprintf(key, sizeof key, "reac.roster.%d.%s", i, field_key(f));
			emit(&e, key, wv, 0);
		}
	}
	/* THE GROUPS THAT LEFT ARE REMOVED, KEY BY KEY. Lowering `reac.roster.n` is not
	 * enough: a client that walks the props rather than the count would keep reading a
	 * segment that is gone, and a blank value would read as a segment with no name. */
	for (int i = r->n_want; i < r->n_pub; i++)
		for (int f = 0; f < REAC_ROSTER_FIELDS; f++) {
			char key[40];
			snprintf(key, sizeof key, "reac.roster.%d.%s", i, field_key(f));
			emit(&e, key, "", 1);
		}

	if (e.over)
		return -1;
	return e.n;
}

void reac_roster_commit(struct reac_roster *r)
{
	memcpy(r->pub, r->want, sizeof r->pub);
	r->n_pub = r->n_want;
}
