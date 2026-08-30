// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Passive REAC sighting classifier + table. See reac_disco.h for the why. */
#include "reac_disco.h"
#include <reac/reac.h>   /* reac_frame_channels / reac_frame_clean_len */

#include <stdio.h>
#include <string.h>

const char *reac_disco_role_name(enum reac_disco_role r)
{
	switch (r) {
	case REAC_DISCO_ROLE_BOX:    return "box";
	case REAC_DISCO_ROLE_MASTER: return "master";
	default:                     return "unknown";
	}
}

/* The box cold-connect JOIN and the master's grant-burst SHARE the cdea 04 03 opcode
 * (REAC-BOX-STATE-DIAGRAM.md), so the kind alone cannot tell them apart — only the
 * JOIN's full signature can. This matcher covers the FULL escalation a real box
 * walks — 0014 -> 0013 -> 0016 -> 001a, sel 00 02 — box-only in every golden
 * (S-1608 2026-07-11; S-4000S on M-200, M-5000 and coldboot: reac_s4000_golden.inc).
 * It deliberately ACCEPTS MORE than the FSM's action matcher in reac_ctrl.c, which
 * grants only on the join and box-ready tags: an IDENTITY record is evidence a box
 * is out there even while the FSM rightly refuses to treat it as the grant
 * trigger.
 *
 * Known residue: the master's grant ECHO (TAG 01 00) is byte-identical to the box's
 * join, so those few frames misfile as BOX (4 in 70913 in the M-200 golden); no
 * per-frame signature can split them — direction resolution is the sighting table's
 * corroboration job (arbitration spec §4, issue #90).
 *
 * THE MATCHER IS ON THE DT1 TAG, NOT THE LENGTH. It used to accept block[2:4] in
 * { 0x0013, 0x0014, 0x0016, 0x001a }, which is the same four containers by their
 * lengths; the tags they carry are the join grant, box-ready and the two identity
 * records. A length test is what the old two-byte "opcode" reading forced, and it
 * survives only while every container happens to be full. */
static int is_box_join(const struct reac_ctrl_parsed *p)
{
	return p->opcode == 0x00 &&           /* the DT1 container's own header byte */
	       (p->dt1_tag == REAC_DT1_TAG_JOIN ||
	        p->dt1_tag == REAC_DT1_TAG_BOX_READY ||
	        p->dt1_tag == REAC_DT1_TAG_IDENTITY);
}

static enum reac_disco_role role_of(const struct reac_ctrl_parsed *p)
{
	switch (p->kind) {
	case REAC_CTRL_FILLER:
		/* Direction discipline (reac_ctrl.h): a master BROADCASTS downstream, a box
		 * UNICASTS upstream. So a unicast FILLER is a box feeding some master.
		 * A BROADCAST filler is genuinely ambiguous — a box's presence-flood and a
		 * master's downstream audio are byte-identical in kind — and stays unknown.
		 * (reac_ctrl.c:137 reads it as a box flood; that is safe only because the FSM
		 * pairs it with the grant handshake. Discovery has no such corroboration.) */
		return p->is_broadcast ? REAC_DISCO_ROLE_UNKNOWN : REAC_DISCO_ROLE_BOX;
	case REAC_CTRL_MASTER_ANNOUNCE:   /* cfea — master only */
	case REAC_CTRL_MASTER_HB:         /* cdea 01 03 0019 */
		return REAC_DISCO_ROLE_MASTER;
	case REAC_CTRL_BOX_HB:            /* cdea 01 03 0001 */
		return REAC_DISCO_ROLE_BOX;
	case REAC_CTRL_HEADAMP:           /* 04 03, TAG 01 01 — only a console emits
		                           * preamp records (m200-headamp-re/DECODE.md;
		                           * x204 in the S-4000 M-200 golden) */
		return REAC_DISCO_ROLE_MASTER;
	case REAC_CTRL_GRANT:             /* a link-4 record — BOTH directions use it */
		return is_box_join(p) ? REAC_DISCO_ROLE_BOX : REAC_DISCO_ROLE_MASTER;
	case REAC_CTRL_CONFIG_ANNOUNCE:
		/* The box's own declaration (link 1, opcode 0x80/0x82/0x84). It used to
		 * land in the parser's link-1 catch-all and had to be dug back out by
		 * hand, or every self-declaring box filed as a rival master. */
		return REAC_DISCO_ROLE_BOX;
	case REAC_CTRL_SCENE_TRANSFER:
		/* Link 1, opcode 0x00, in any of its four segment states — the master's
		 * enrolment push. This is what the old code saw as "the master hunt
		 * sub-state families 01 00 001a / 01 01 0018 / 01 02 000e" and named
		 * MASTER off block[1] <= 2: three segment states of one transfer read as
		 * three opcodes. Master-only, and now for the reason rather than the
		 * pattern. */
		return REAC_DISCO_ROLE_MASTER;
	default:
		/* Arbitration makes this verdict load-bearing, so only CAPTURED
		 * signatures name a role and everything else stays a visible sighting
		 * with no role. That now covers GROUP_MAP (link 1, opcode 0x10, which
		 * only the S-4000S image even handles), RECORD_FRAGMENT (half a record:
		 * its tag and checksum are not readable until the other half arrives),
		 * LINK2, SPLIT_ANNOUNCE (real gear, none captured, §14.1) and
		 * UNKNOWN_CTRL. The old code answered MASTER for anything on link 1
		 * whose block[1] was <= 2 whatever its opcode; a frame nobody has
		 * captured must not be able to flip the segment's topology
		 * (arbitration spec §4). */
		return REAC_DISCO_ROLE_UNKNOWN;
	}
}

int reac_disco_classify(const uint8_t *frame, size_t len, const uint8_t our_mac[6],
                        struct reac_disco_sighting *out)
{
	struct reac_ctrl_parsed p;
	if (reac_ctrl_parse(frame, len, &p) == REAC_CTRL_NONE)
		return -1;                       /* not a 0x8819 frame: not evidence */
	/* The Roland OUI is the "is this REAC gear at all" gate — the one test that makes a
	 * sighting a fact rather than a packet count. */
	if (p.src[0] != 0x00 || p.src[1] != 0x40 || p.src[2] != 0xab)
		return -1;
	if (memcmp(p.src, our_mac, 6) == 0)
		return -1;                       /* our own echo (PACKET_IGNORE_OUTGOING is
		                                  * best-effort; hubs and loopbacks echo) */
	/* A cdea/cfea frame that fails the checksum is corrupt — not a device with a bad
	 * byte. FILLER (type 0000) is checksum-exempt: its block is the audio descriptor. */
	if (p.kind != REAC_CTRL_FILLER && reac_ctrl_checksum_verify(frame) != 0)
		return -1;

	memset(out, 0, sizeof *out);
	memcpy(out->mac, p.src, 6);
	out->role = role_of(&p);
	/* Byte-exact config-block match or NULL. Never reac_box_model_by_channels: its
	 * S-1608 default (reac_ctrl.c:394) would name a box that was never identified. */
	out->model = reac_ctrl_identify_box(frame, len);
	/* The geometry, straight off the length: what the peer IS, beside what it claims. */
	out->channels = reac_frame_channels(reac_frame_clean_len(len));
	return 0;
}

int reac_disco_model_index(const struct reac_box_model *m)
{
	if (!m)
		return -1;
	size_t count = 0;
	const struct reac_box_model *base = reac_box_model_table(&count);
	for (size_t i = 0; i < count; i++)
		if (&base[i] == m)
			return (int)i;
	return -1;
}

const struct reac_box_model *reac_disco_model_by_index(int idx)
{
	if (idx < 0)
		return NULL;
	size_t count = 0;
	const struct reac_box_model *base = reac_box_model_table(&count);
	if ((size_t)idx >= count)
		return NULL;
	return &base[idx];
}

void reac_disco_gate_init(struct reac_disco_gate *g)
{
	memset(g, 0, sizeof *g);
}

int reac_disco_gate_should_push(struct reac_disco_gate *g,
                                const struct reac_disco_sighting *s, uint64_t now_ns)
{
	const int model_idx = reac_disco_model_index(s->model);

	for (int i = 0; i < g->n; i++) {
		struct reac_disco_gate_entry *e = &g->e[i];
		if (memcmp(e->mac, s->mac, 6) != 0)
			continue;
		/* An EDGE: the facts about this MAC sharpened. Never gated by the refresh
		 * window — the operator should see a box identify itself immediately, not up
		 * to a second later. Facts only sharpen (unknown -> known), so an ambiguous
		 * frame arriving after a definite one is not an edge. */
		if ((s->role != REAC_DISCO_ROLE_UNKNOWN && s->role != e->role) ||
		    (model_idx >= 0 && model_idx != e->model_idx)) {
			if (s->role != REAC_DISCO_ROLE_UNKNOWN)
				e->role = s->role;
			if (model_idx >= 0)
				e->model_idx = model_idx;
			e->last_push_ns = now_ns;
			return 1;
		}
		/* Otherwise: one refresh per window, to drive main-thread aging. The guard
		 * against a non-monotonic clock keeps a backwards jump from wedging refreshes
		 * (which would age a live box out). */
		if (now_ns < e->last_push_ns ||
		    now_ns - e->last_push_ns >= REAC_DISCO_REFRESH_NS) {
			e->last_push_ns = now_ns;
			return 1;
		}
		return 0;
	}

	if (g->n >= REAC_DISCO_MAX)
		return 0;   /* saturated: the table reports overflow, the ring stays sane */

	struct reac_disco_gate_entry *e = &g->e[g->n++];
	memcpy(e->mac, s->mac, 6);
	e->role = s->role;
	e->model_idx = model_idx;
	e->last_push_ns = now_ns;
	return 1;   /* a MAC never seen before is always worth a slot */
}

void reac_disco_table_init(struct reac_disco_table *t)
{
	memset(t, 0, sizeof *t);
}

static struct reac_disco_entry *find(struct reac_disco_table *t, const uint8_t mac[6])
{
	for (int i = 0; i < t->n; i++)
		if (memcmp(t->e[i].mac, mac, 6) == 0)
			return &t->e[i];
	return NULL;
}

int reac_disco_table_observe(struct reac_disco_table *t,
                             const struct reac_disco_sighting *s,
                             int owned, uint64_t now_ns)
{
	struct reac_disco_entry *e = find(t, s->mac);
	if (e) {
		e->last_seen_ns = now_ns;        /* liveness always advances… */
		int changed = 0;
		/* …but only a real upgrade is a CHANGE. A re-sighting that bumped seq would
		 * have openmixer re-reading an identical list at wire rate. Facts only ever
		 * sharpen: an ambiguous role or an unidentified model is replaced once it is
		 * known, never downgraded back by a later ambiguous frame. */
		if (s->role != REAC_DISCO_ROLE_UNKNOWN && e->role != s->role) {
			e->role = s->role;
			changed = 1;
		}
		if (s->model && e->model != s->model) {
			e->model = s->model;
		if (s->channels > e->channels) {
			/* WIDEST WINS. Control frames carry no audio geometry, so a peer's data frames
			 * are what answer; last-wins would let one short control frame erase them. */
			e->channels = s->channels;
			changed = 1;
		}
			changed = 1;
		}
		if (owned && !e->owned) {
			e->owned = 1;
			changed = 1;
		}
		if (changed)
			t->seq++;
		return changed;
	}

	if (t->n >= REAC_DISCO_MAX) {
		/* Saturate rather than evict, and record that we did: a full table is a
		 * PARTIAL view, and the reader is told so instead of being handed a list that
		 * looks complete. */
		if (!t->overflowed) {
			t->overflowed = 1;
			t->seq++;
		}
		return 0;
	}

	e = &t->e[t->n++];
	memset(e, 0, sizeof *e);
	memcpy(e->mac, s->mac, 6);
	e->role = s->role;
	e->model = s->model;
	e->channels = s->channels;
	e->owned = owned ? 1 : 0;
	e->first_seen_ns = now_ns;
	e->last_seen_ns = now_ns;
	t->seq++;
	return 1;
}

int reac_disco_table_age(struct reac_disco_table *t, uint64_t now_ns)
{
	int removed = 0;
	for (int i = 0; i < t->n; ) {
		/* Guard the subtraction: a non-monotonic or replayed clock must not wrap into
		 * a colossal age and withdraw every live device. */
		if (now_ns > t->e[i].last_seen_ns &&
		    now_ns - t->e[i].last_seen_ns > REAC_DISCO_STALE_NS) {
			t->e[i] = t->e[t->n - 1];   /* order is not meaningful */
			t->n--;
			removed++;
		} else {
			i++;
		}
	}
	if (removed)
		t->seq++;
	return removed;
}

int reac_disco_table_json(const struct reac_disco_table *t, uint64_t now_ns,
                          char *buf, size_t cap)
{
	size_t off = 0;

	/* Every field is written through this: a would-be overflow aborts the WHOLE
	 * serialization (caller publishes nothing) rather than emitting a truncated array
	 * that still parses — as a shorter, wrong device list. */
#define PUT(...) do { \
		int _w = snprintf(buf + off, cap - off, __VA_ARGS__); \
		if (_w < 0 || (size_t)_w >= cap - off) return -1; \
		off += (size_t)_w; \
	} while (0)

	if (cap < 3)
		return -1;
	PUT("[");
	for (int i = 0; i < t->n; i++) {
		const struct reac_disco_entry *e = &t->e[i];
		uint64_t age_ns = now_ns > e->last_seen_ns ? now_ns - e->last_seen_ns : 0;
		PUT("%s{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"", i ? "," : "",
		    e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
		PUT(",\"role\":\"%s\"", reac_disco_role_name(e->role));
		PUT(",\"model\":\"%s\"", e->model ? e->model->token : "unknown");
		if (e->model)
			PUT(",\"width\":\"%dx%d\"", e->model->in_ch, e->model->out_ch);
		else
			PUT(",\"width\":\"0x0\"");   /* unidentified: no width is claimed */
		PUT(",\"owned\":%s", e->owned ? "true" : "false");
		PUT(",\"age_ms\":%llu}", (unsigned long long)(age_ns / 1000000ULL));
	}
	PUT("]");
#undef PUT
	return (int)off;
}
