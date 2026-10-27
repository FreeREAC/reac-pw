// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* THE SEED CORPUS OF fuzz/, WRITTEN FROM THIS FILE ALONE (make fuzz-corpus; make
 * fuzz-corpus-check compares). Each seed is spelled in the input grammar its harness's
 * header documents, by the helpers below, and named for what it carries; the names are
 * the `input:` lines of the records. Nothing here reads a network, a capture or the
 * clock, so the corpus is the same bytes on every machine.
 *
 * Usage: gen-corpus <directory>   (writes <directory>/<harness>/<seed>) */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *g_dir;

struct g {
	uint8_t b[16384];
	size_t n;
};

static void put8(struct g *g, unsigned v)
{
	if (g->n < sizeof g->b)
		g->b[g->n++] = (uint8_t)v;
}

static void put16(struct g *g, unsigned v)
{
	put8(g, v & 0xff);
	put8(g, v >> 8 & 0xff);
}

static void put32(struct g *g, uint32_t v)
{
	put16(g, v & 0xffff);
	put16(g, v >> 16);
}

static void puts_(struct g *g, const char *s)
{
	while (*s)
		put8(g, (uint8_t)*s++);
}

static void putn(struct g *g, const void *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		put8(g, ((const uint8_t *)p)[i]);
}

static int g_failed;

static void emit(const char *harness, const char *name, const struct g *g)
{
	char path[512];
	snprintf(path, sizeof path, "%s/%s", g_dir, harness);
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		perror(path);
		g_failed = 1;
		return;
	}
	snprintf(path, sizeof path, "%s/%s/%s", g_dir, harness, name);
	FILE *f = fopen(path, "wb");
	if (!f || fwrite(g->b, 1, g->n, f) != g->n || fclose(f) != 0) {
		perror(path);
		g_failed = 1;
	}
}

/* ---- fuzz_pin: [flags][spec] ---------------------------------------------------------- */

static void pin(const char *name, unsigned flags, const char *spec)
{
	struct g g = { .n = 0 };
	put8(&g, flags);
	puts_(&g, spec);
	emit("pin", name, &g);
}

static void gen_pin(void)
{
	pin("s1608-left", 0, "s1608:Left");
	pin("s0808", 0, "s0808");
	pin("prefix-s16", 0, "s16:Stage");
	pin("s1608-empty-label", 0, "s1608:");
	pin("s4000s-0832-colons", 0, "s4000s-0832:Stage: left");
	pin("unknown-model", 0, "s9999:x");
	pin("token-with-more", 0, "s1608x");
	pin("colon-only", 0, ":Label");
	pin("empty", 0, "");
	pin("null-spec", 1, "s1608");
	pin("null-outs", 6, "s1608:Left");
}

/* ---- the Props object of fuzz/pod.h --------------------------------------------------- */

/* A Props object with one SPA_PROP_params Struct of `count` pods; the pods follow. */
static void props(struct g *g, unsigned shape, unsigned count)
{
	put8(g, shape);
	put8(g, 1);       /* one property */
	put8(g, 1);       /* SPA_PROP_params, a Struct */
	put8(g, count);
}

/* the pods, as ent_build reads them */
static void hkey(struct g *g, unsigned ch, unsigned param)
{
	put8(g, 0);
	put8(g, ch);
	put8(g, param);
}

static void voc(struct g *g, unsigned i)
{
	put8(g, 1);
	put8(g, i);
}

static void vint(struct g *g, int32_t v)
{
	put8(g, 2);
	put8(g, 1);
	put32(g, (uint32_t)v);
}

static void vflt(struct g *g, float v)
{
	uint32_t bits;
	memcpy(&bits, &v, 4);
	put8(g, 3);
	put8(g, 1);
	put32(g, bits);
}

static void vbool(struct g *g, unsigned v)
{
	put8(g, 4);
	put8(g, v);
}

/* the byte after the object: no overwrite */
static void tail(struct g *g)
{
	put8(g, 0);
}

/* The vocabulary of ent_build's kind 1, by index. */
enum { V_RATE, V_ROLE, V_HA_PREFIX, V_RATE_SP, V_RATE_UP, V_OTHER, V_HA_NODOT, V_ROLE_X,
       V_RAT };
/* The params of ent_headamp_key, by index; | 0x80 leaves the '.' out. */
enum { P_PHANTOM, P_PAD, P_SENS, P_PAD_UP, P_SENSX, P_EMPTY, P_PHANTOM_X, P_SENS_SP };
/* The odd channels of ent_headamp_key: a channel byte of 200 or more picks odd_ch[c % 14]. */
#define CH_MINUS_ONE 210
#define CH_PLUS_THREE 211
#define CH_HEX_ONE 208

/* ---- fuzz_headamp: [flags][cap][object] ------------------------------------------------ */

static void gen_headamp(void)
{
	struct g g;

	g.n = 0;
	put8(&g, 0);
	put8(&g, 8);
	props(&g, 0, 12);
	hkey(&g, 5, P_PHANTOM); vint(&g, 1);
	hkey(&g, 5, P_SENS); vint(&g, 0x10);
	hkey(&g, 6, P_PAD); vbool(&g, 1);
	voc(&g, V_OTHER); vint(&g, 99);
	hkey(&g, 48, P_PHANTOM); vint(&g, 1);
	hkey(&g, 7, P_SENS); vint(&g, 0x38);
	tail(&g);
	emit("headamp", "cells-and-refusals", &g);

	g.n = 0;
	put8(&g, 0);
	put8(&g, 1);
	props(&g, 0, 4);
	hkey(&g, 1, P_PHANTOM); vint(&g, 1);
	hkey(&g, 2, P_PHANTOM); vint(&g, 1);
	tail(&g);
	emit("headamp", "cap-one-two-cells", &g);

	g.n = 0;
	put8(&g, 0);
	put8(&g, 8);
	props(&g, 0, 2);
	hkey(&g, 48, P_PHANTOM); vint(&g, 1);
	tail(&g);
	emit("headamp", "channel-48", &g);

	g.n = 0;
	put8(&g, 0);
	put8(&g, 8);
	props(&g, 0, 2);
	hkey(&g, 3, P_PHANTOM); vint(&g, 2);
	tail(&g);
	emit("headamp", "phantom-two", &g);

	g.n = 0;
	put8(&g, 0);
	put8(&g, 8);
	props(&g, 0, 2);
	hkey(&g, 3, P_PHANTOM); vint(&g, 256);
	tail(&g);
	emit("headamp", "phantom-256", &g);

	g.n = 0;
	put8(&g, 0);
	put8(&g, 8);
	props(&g, 0, 4);
	hkey(&g, 3, P_SENS); vint(&g, 0x37);
	hkey(&g, 4, P_SENS); vflt(&g, 255.5f);
	tail(&g);
	emit("headamp", "sens-edges", &g);

	g.n = 0;
	put8(&g, 0);
	put8(&g, 8);
	props(&g, 0, 2);
	hkey(&g, CH_HEX_ONE, P_PHANTOM); vint(&g, 1);
	tail(&g);
	emit("headamp", "hex-channel", &g);

	g.n = 0;
	put8(&g, 0);
	put8(&g, 8);
	props(&g, 0, 6);
	hkey(&g, 3, P_SENSX); vint(&g, 1);
	hkey(&g, 48, P_PHANTOM); vint(&g, 1);
	hkey(&g, 2, P_PHANTOM); vint(&g, 1);
	tail(&g);
	emit("headamp", "refusal-order", &g);

	g.n = 0;
	put8(&g, 0);
	put8(&g, 8);
	props(&g, 0, 4);
	hkey(&g, 3, P_SENSX); vint(&g, 1);
	hkey(&g, 48, P_PHANTOM); vint(&g, 1);
	tail(&g);
	emit("headamp", "refused-only", &g);

	g.n = 0;
	put8(&g, 0);
	put8(&g, 8);
	props(&g, 6, 2);
	hkey(&g, 1, P_PHANTOM); vint(&g, 1);
	tail(&g);
	emit("headamp", "not-props", &g);

	g.n = 0;
	put8(&g, 0);
	put8(&g, 8);
	put8(&g, 0);
	put8(&g, 1);
	put8(&g, 0x11);   /* SPA_PROP_params whose value is an Int */
	tail(&g);
	emit("headamp", "params-not-a-struct", &g);

	g.n = 0;
	put8(&g, 1);
	put8(&g, 8);
	props(&g, 0, 2);
	hkey(&g, 1, P_PHANTOM); vint(&g, 1);
	tail(&g);
	emit("headamp", "null-object", &g);

	g.n = 0;
	put8(&g, 0);
	put8(&g, 8);
	props(&g, 0, 8);
	hkey(&g, 4, P_PHANTOM); vflt(&g, 0.49999997f);
	hkey(&g, 8, P_SENS); vflt(&g, 12.4f);
	hkey(&g, 5, P_PHANTOM);
	put8(&g, 3); put8(&g, 254);   /* a NaN */
	hkey(&g, CH_PLUS_THREE, P_PAD); vflt(&g, 1.0f);
	tail(&g);
	emit("headamp", "float-cells", &g);

	/* a key slot holding a non-String re-syncs on the next String; and pods of every
	 * other shape */
	g.n = 0;
	put8(&g, 0);
	put8(&g, 9);
	props(&g, 0, 16);
	vint(&g, 1234);
	hkey(&g, 3, P_PHANTOM); vint(&g, 1);
	put8(&g, 5); put8(&g, 5); puts_(&g, "abcde");       /* a String of raw bytes */
	put8(&g, 6); put8(&g, 0); put32(&g, 1); put32(&g, 0);   /* a Long */
	put8(&g, 7); put8(&g, 8); put8(&g, 3); puts_(&g, "abc");   /* String type, size 3 */
	put8(&g, 8); put8(&g, 3);                         /* a String with no body */
	put8(&g, 9);                                      /* a Struct */
	hkey(&g, 9, P_PAD | 0x80); vint(&g, 1);
	hkey(&g, CH_MINUS_ONE, P_PAD); vint(&g, 1);
	voc(&g, V_HA_PREFIX); vint(&g, 1);
	put8(&g, 135); put8(&g, 4); puts_(&g, "abcd");   /* a String with no NUL */
	tail(&g);
	emit("headamp", "pods-of-every-shape", &g);

	g.n = 0;
	put8(&g, 0);
	put8(&g, 8);
	props(&g, 0, 4);
	hkey(&g, 1, P_PHANTOM); vint(&g, 1);
	hkey(&g, 2, P_SENS); vint(&g, 5);
	put8(&g, 15); put16(&g, 20); put8(&g, 4); put32(&g, 0xffffffffu);
	emit("headamp", "overwritten", &g);

	/* a head-amp key whose value is no number */
	g.n = 0;
	put8(&g, 0);
	put8(&g, 8);
	props(&g, 0, 2);
	hkey(&g, 3, P_PHANTOM); voc(&g, V_OTHER);
	tail(&g);
	emit("headamp", "value-a-string", &g);
}

/* A Props object whose params Struct is one key and one value; `shape` 6 is PropInfo. */
struct kv {
	int key;           /* a vocabulary index, or -1 for none */
	int kind;          /* 'i' Int, 'f' Float, 'b' Bool, 's' a String */
	int32_t i;
	float f;
	const char *s;
};

static void kvs(struct g *g, unsigned shape, const struct kv *p, int n)
{
	props(g, shape, (unsigned)(2 * n));
	for (int k = 0; k < n; k++) {
		voc(g, (unsigned)p[k].key);
		switch (p[k].kind) {
		case 'i': vint(g, p[k].i); break;
		case 'f': vflt(g, p[k].f); break;
		case 'b': vbool(g, (unsigned)p[k].i); break;
		default:
			put8(g, 5);
			put8(g, (unsigned)strlen(p[k].s));
			puts_(g, p[k].s);
			break;
		}
	}
	tail(g);
}

static void params_int(struct g *g)
{
	put8(g, 0);
	put8(g, 1);
	put8(g, 0x11);
	tail(g);
}

/* ---- fuzz_rate: [flags][mask][buflen][u32 other][object] ------------------------------- */

static void rate(const char *name, unsigned flags, unsigned mask, unsigned buflen,
                 uint32_t other, unsigned shape, const struct kv *p, int n)
{
	struct g g = { .n = 0 };
	put8(&g, flags);
	put8(&g, mask);
	put8(&g, buflen);
	put32(&g, other);
	if (n < 0)
		params_int(&g);
	else
		kvs(&g, shape, p, n);
	emit("rate", name, &g);
}

static void gen_rate(void)
{
	const struct kv r48[] = { { V_RATE, 'i', 48000, 0, 0 } };
	const struct kv r96[] = { { V_RATE, 'i', 96000, 0, 0 } };
	const struct kv r44[] = { { V_RATE, 'i', 44100, 0, 0 } };
	const struct kv neg[] = { { V_RATE, 'f', 0, -0.7f, 0 } };
	const struct kv after[] = { { V_OTHER, 'i', 7, 0, 0 }, { V_RATE, 'i', 48000, 0, 0 } };
	const struct kv huge[] = { { V_RATE, 'f', 0, -1e10f, 0 } };
	const struct kv boolv[] = { { V_RATE, 'b', 1, 0, 0 } };
	const struct kv near[] = { { V_RAT, 'i', 48000, 0, 0 }, { V_RATE_SP, 'i', 1, 0, 0 },
	                           { V_RATE_UP, 'i', 1, 0, 0 } };
	const struct kv half[] = { { V_RATE, 'f', 0, 47999.5f, 0 } };
	const struct kv first[] = { { V_RATE, 'i', 44100, 0, 0 }, { V_RATE, 'i', 96000, 0, 0 } };
	const struct kv str[] = { { V_RATE, 's', 0, 0, "48000" } };
	const struct kv inf[] = { { V_RATE, 'f', 0, __builtin_inff(), 0 } };
	rate("rate-48000", 0, 7, 3, 44100, 0, r48, 1);
	rate("rate-96000", 0, 7, 20, 96000, 0, r96, 1);
	rate("rate-44100", 0, 7, 0, 44100, 0, r44, 1);
	rate("rate-not-drivable", 0, 5, 20, 1, 0, r48, 1);
	rate("slave-asks-48000", 2, 7, 6, 48000, 0, r48, 1);
	rate("rate-float-negative", 0, 7, 8, 0, 0, neg, 1);
	rate("rate-after-other-key", 0, 2, 8, 0, 0, after, 2);
	rate("rate-float-huge", 0, 7, 8, 0, 0, huge, 1);
	rate("rate-float-infinite", 0, 7, 8, 0, 0, inf, 1);
	rate("rate-bool", 0, 7, 8, 0, 0, boolv, 1);
	rate("rate-string", 0, 7, 8, 0, 0, str, 1);
	rate("not-props-rate", 0, 7, 8, 0, 6, r48, 1);
	rate("params-not-a-struct", 0, 7, 8, 0, 0, NULL, -1);
	rate("near-keys", 0, 7, 8, 0, 0, near, 3);
	rate("rate-float-half", 0, 3, 23, 0, 0, half, 1);
	rate("rate-first-wins", 1, 0, 1, 48000, 0, first, 2);
}

/* ---- fuzz_role: [flags][object] -------------------------------------------------------- */

static void role(const char *name, unsigned flags, unsigned shape, const struct kv *p, int n)
{
	struct g g = { .n = 0 };
	put8(&g, flags);
	if (n < 0)
		params_int(&g);
	else
		kvs(&g, shape, p, n);
	emit("role", name, &g);
}

static void gen_role(void)
{
	const struct kv m[] = { { V_ROLE, 'i', 0, 0, 0 } };
	const struct kv s[] = { { V_ROLE, 'f', 0, 1.0f, 0 } };
	const struct kv below[] = { { V_ROLE, 'f', 0, 0.49999997f, 0 } };
	const struct kv two[] = { { V_ROLE, 'i', 2, 0, 0 } };
	const struct kv neg[] = { { V_ROLE, 'f', 0, -0.7f, 0 } };
	const struct kv one[] = { { V_ROLE, 'i', 1, 0, 0 } };
	const struct kv near[] = { { V_ROLE_X, 'i', 1, 0, 0 }, { V_RATE, 'i', 1, 0, 0 } };
	const struct kv str[] = { { V_ROLE, 's', 0, 0, "master" } };
	role("role-master", 0, 0, m, 1);
	role("role-slave-float", 2, 0, s, 1);
	role("role-float-below-half", 2, 0, below, 1);
	role("role-two", 0, 0, two, 1);
	role("role-float-negative", 0, 0, neg, 1);
	role("not-props-role", 0, 6, one, 1);
	role("params-not-a-struct", 0, 0, NULL, -1);
	role("near-role-keys", 0, 0, near, 2);
	role("role-string", 0, 0, str, 1);
	role("null-object", 1, 0, one, 1);
	{
		const struct kv nan[] = { { V_ROLE, 'f', 0, __builtin_nanf(""), 0 } };
		role("role-nan", 0, 0, nan, 1);
	}
}

/* ---- fuzz_qdisc: [reads] then per read [ifindex][flags][errno][end errno][chunks] -------- */

static void qread(struct g *g, int ifindex, unsigned flags, unsigned nchunks)
{
	put8(g, (unsigned)(uint8_t)ifindex);
	put8(g, flags);
	put8(g, 0);   /* ENOBUFS */
	put8(g, 0);
	put8(g, nchunks);
}

/* a chunk of `n` messages spelled by msg_build */
static void qchunk(struct g *g, unsigned n)
{
	put8(g, 0);
	put8(g, n);
}

/* an etf (or `kind_attr`) qdisc of the device asked for (other: another) with `nattr`
 * attributes to follow */
static void qmsg(struct g *g, int other, unsigned nattr)
{
	put8(g, 0);
	put8(g, other ? 1 : 0);
	put8(g, nattr);
}

static void qkind_etf(struct g *g)
{
	put8(g, 0);
}

/* TCA_STATS2 with a basic block and a queue block */
static void qstats2(struct g *g, uint32_t bytes, uint32_t packets, uint32_t drops,
                    uint32_t over)
{
	put8(g, 2);
	put8(g, 2);
	put8(g, 0); put32(g, bytes); put32(g, packets); put32(g, 0);
	put8(g, 1); put32(g, 0); put32(g, drops); put32(g, over);
}

/* TCA_STATS, whole (3) or too short (4) */
static void qstats1(struct g *g, unsigned a, uint32_t bytes, uint32_t packets, uint32_t drops,
                    uint32_t over)
{
	put8(g, a);
	put32(g, bytes); put32(g, packets); put32(g, drops); put32(g, over);
}

static void qdone(struct g *g)
{
	put8(g, 1);
}

static void qend(struct g *g, unsigned lead, unsigned fps)
{
	put16(g, lead);
	put8(g, fps);
}

static void gen_qdisc(void)
{
	struct g g;

	g.n = 0;
	put8(&g, 1);
	qread(&g, 3, 0, 1);
	qchunk(&g, 3);
	qmsg(&g, 0, 2); qkind_etf(&g); qstats2(&g, 1000, 10, 2, 3);
	qmsg(&g, 0, 2); qkind_etf(&g); qstats2(&g, 500, 5, 4, 1);
	qdone(&g);
	qend(&g, 2500, 0);
	emit("qdisc", "two-etf-qdiscs", &g);

	g.n = 0;
	put8(&g, 1);
	qread(&g, 3, 0, 1);
	qchunk(&g, 3);
	qmsg(&g, 1, 2); qkind_etf(&g); qstats2(&g, 1000, 10, 2, 3);
	qmsg(&g, 0, 2); qkind_etf(&g); qstats2(&g, 7, 1, 1, 1);
	qdone(&g);
	qend(&g, 400, 1);
	emit("qdisc", "other-device", &g);

	g.n = 0;
	put8(&g, 1);
	qread(&g, 3, 0, 1);
	qchunk(&g, 5);
	qmsg(&g, 0, 2); put8(&g, 25); qstats2(&g, 1000, 10, 2, 3);    /* "etfx" */
	qmsg(&g, 0, 2); put8(&g, 1); qstats2(&g, 1000, 10, 2, 3);     /* "fq_codel" */
	qmsg(&g, 0, 2); put8(&g, 133); qstats2(&g, 1000, 10, 2, 3);   /* "etf", no NUL */
	qmsg(&g, 0, 2); qkind_etf(&g); qstats2(&g, 1, 1, 1, 1);
	qdone(&g);
	qend(&g, 2500, 4);
	emit("qdisc", "other-kinds", &g);

	g.n = 0;
	put8(&g, 1);
	qread(&g, 3, 0, 1);
	qchunk(&g, 2);
	qmsg(&g, 0, 2); qkind_etf(&g); qstats1(&g, 4, 900, 9, 4, 6);
	qdone(&g);
	qend(&g, 2500, 1);
	emit("qdisc", "stats1-short", &g);

	g.n = 0;
	put8(&g, 1);
	qread(&g, 3, 0, 1);
	qchunk(&g, 2);
	qmsg(&g, 0, 2); qkind_etf(&g); qstats1(&g, 3, 900, 9, 4, 6);
	qdone(&g);
	qend(&g, 2500, 5);
	emit("qdisc", "stats1-only", &g);

	g.n = 0;
	put8(&g, 1);
	qread(&g, 3, 0, 1);
	qchunk(&g, 2);
	qdone(&g);
	qmsg(&g, 0, 2); qkind_etf(&g); qstats2(&g, 1000, 10, 2, 3);
	qend(&g, 2500, 0);
	emit("qdisc", "done-then-more", &g);

	g.n = 0;
	put8(&g, 1);
	qread(&g, 3, 0, 1);
	qchunk(&g, 1);
	put8(&g, 2); put8(&g, 0);   /* an ACK: no dump at all */
	qend(&g, 2500, 0);
	emit("qdisc", "error-ack", &g);

	g.n = 0;
	put8(&g, 1);
	qread(&g, 3, 0, 1);
	qchunk(&g, 1);
	put8(&g, 2); put8(&g, 196);   /* -EPERM, cut after its code */
	qend(&g, 2500, 0);
	emit("qdisc", "error-short", &g);

	g.n = 0;
	put8(&g, 1);
	qread(&g, 3, 0, 1);
	qchunk(&g, 1);
	put8(&g, 2); put8(&g, 4);     /* a positive code */
	qend(&g, 2500, 0);
	emit("qdisc", "error-positive", &g);

	g.n = 0;
	put8(&g, 1);
	qread(&g, 3, 0, 1);
	qchunk(&g, 1);
	qmsg(&g, 0, 2); qkind_etf(&g); qstats2(&g, 1000, 10, 2, 3);
	qend(&g, 2500, 0);
	emit("qdisc", "replies-end", &g);

	g.n = 0;
	put8(&g, 1);
	qread(&g, 3, 1, 0);
	qend(&g, 2500, 0);
	emit("qdisc", "socket-fails", &g);

	g.n = 0;
	put8(&g, 1);
	qread(&g, 3, 2, 0);
	qend(&g, 2500, 0);
	emit("qdisc", "send-fails", &g);

	g.n = 0;
	put8(&g, 2);
	qread(&g, 0, 0, 0);
	qread(&g, 5, 4, 0);
	qend(&g, 100, 0);
	emit("qdisc", "index-zero", &g);

	/* two chunks, the DONE in the second; a raw chunk of a header too short */
	g.n = 0;
	put8(&g, 1);
	qread(&g, 9, 0, 3);
	qchunk(&g, 1);
	qmsg(&g, 0, 2); qkind_etf(&g); qstats2(&g, 1, 2, 3, 4);
	put8(&g, 1); put16(&g, 7); putn(&g, "\x10\0\0\0\x24\0\0", 7);
	qchunk(&g, 2);
	qmsg(&g, 0, 1); qkind_etf(&g);
	qdone(&g);
	qend(&g, 65535, 4);
	emit("qdisc", "multi-chunk", &g);

	/* the last message of the reply is not padded */
	g.n = 0;
	put8(&g, 1);
	qread(&g, 3, 0, 1);
	qchunk(&g, 1);
	put8(&g, 6); put8(&g, 0); put8(&g, 1); qkind_etf(&g);
	qend(&g, 2500, 0);
	emit("qdisc", "unpadded-last", &g);

	/* a kind attribute with no payload at all */
	g.n = 0;
	put8(&g, 1);
	qread(&g, 3, 0, 1);
	qchunk(&g, 2);
	qmsg(&g, 0, 1);
	for (unsigned a = 128; a < 256; a++)
		if (a % 6 == 1 && (a >> 3) % 5 == 2) {
			put8(&g, a);
			break;
		}
	qdone(&g);
	qend(&g, 2500, 0);
	emit("qdisc", "kind-empty", &g);

	/* a qdisc message too short for its tcmsg, another type, a header too short, an
	 * unpadded last message */
	g.n = 0;
	put8(&g, 3);
	qread(&g, 3, 0, 1);
	qchunk(&g, 3);
	put8(&g, 3);
	put8(&g, 4);
	put8(&g, 5);
	qread(&g, 3, 0, 1);
	qchunk(&g, 1);
	put8(&g, 7);
	qread(&g, 3, 0, 1);
	qchunk(&g, 2);
	qmsg(&g, 0, 1); qkind_etf(&g);
	put8(&g, 6); put8(&g, 0); put8(&g, 1); qkind_etf(&g);
	qend(&g, 3000, 3);
	emit("qdisc", "malformed", &g);
}

/* ---- fuzz_link: operations, each [op] then its fields and the kernel ------------------- */

/* The texts of fuzz_link.c's `texts`, by index. */
enum { T_EMPTY, T_SERVED, T_MINTED, T_HOST, T_PREFIX, T_SERVED2, T_REACPW, T_VLAN, T_VETH,
       T_VLANX, T_VLA };
/* attribute kinds of msg_build */
enum { A_ALIAS, A_LONG_ALIAS, A_KIND, A_KIND_DATA, A_IFNAME, A_OTHER };

/* the attribute byte of `type` holding text `t`; `bare` leaves its NUL off */
static void lattr(struct g *g, unsigned type, unsigned t, int bare)
{
	for (unsigned a = 0; a < 128; a++)
		if (a % 6 == type && (a >> 3) % 11 == t) {
			put8(g, a | (bare ? 0x80u : 0));
			return;
		}
}

/* the kernel of one operation: no failure, ENOBUFS once the replies end, `n` chunks */
static void lkernel(struct g *g, unsigned flags, unsigned n)
{
	put8(g, flags);
	put8(g, 0);
	put8(g, 0);
	put8(g, n);
}

/* a chunk holding one RTM_NEWLINK with `nattr` attributes to follow */
static void lnewlink(struct g *g, unsigned nattr)
{
	put8(g, 0);
	put8(g, 1);
	put8(g, 0);
	put8(g, nattr);
}

/* a chunk holding one ACK */
static void lack(struct g *g)
{
	put8(g, 0);
	put8(g, 1);
	put8(g, 1);
}

/* a chunk holding one error, by msg_build's code byte */
static void lerror(struct g *g, unsigned c)
{
	put8(g, 0);
	put8(g, 1);
	put8(g, 2);
	put8(g, c);
}

static void lread(struct g *g, unsigned name, unsigned kc, unsigned ac)
{
	put8(g, 0);
	put8(g, name);
	put8(g, kc);
	put8(g, ac);
}

static void lmark(struct g *g, unsigned name)
{
	put8(g, 2);
	put8(g, name);
}

static void gen_link(void)
{
	struct g g;

	g.n = 0;
	lread(&g, 3, 20, 30);
	lkernel(&g, 0, 1);
	lnewlink(&g, 1); lattr(&g, A_ALIAS, T_SERVED, 0);
	emit("link", "read-served", &g);

	g.n = 0;
	lread(&g, 4, 20, 30);
	lkernel(&g, 0, 1);
	lnewlink(&g, 2); lattr(&g, A_KIND_DATA, T_VLAN, 0); lattr(&g, A_ALIAS, T_HOST, 0);
	emit("link", "read-vlan-with-data", &g);

	g.n = 0;
	lread(&g, 5, 10, 10);
	lkernel(&g, 0, 1);
	lnewlink(&g, 1); lattr(&g, A_IFNAME, T_EMPTY, 0);
	emit("link", "read-plain-nic", &g);

	g.n = 0;
	lread(&g, 5, 10, 10);
	lkernel(&g, 0, 1);
	lerror(&g, 192);   /* -EPERM, cut after its code */
	emit("link", "read-error-short", &g);

	/* a message of another type whose length is not padded ends the chunk */
	g.n = 0;
	lread(&g, 5, 10, 10);
	lkernel(&g, 0, 1);
	put8(&g, 1);
	put16(&g, 21);
	put32(&g, 21); put16(&g, 20); put16(&g, 0); put32(&g, 1); put32(&g, 0);
	puts_(&g, "abcde");
	emit("link", "read-unpadded-last", &g);

	g.n = 0;
	lread(&g, 6, 10, 20);
	lkernel(&g, 0, 1);
	put8(&g, 0); put8(&g, 2); put8(&g, 4);
	put8(&g, 0); put8(&g, 1); lattr(&g, A_ALIAS, T_HOST, 0);
	emit("link", "read-after-other", &g);

	g.n = 0;
	lread(&g, 7, 4, 10);
	lkernel(&g, 0, 1);
	lnewlink(&g, 3); lattr(&g, A_ALIAS, T_HOST, 0); lattr(&g, A_KIND, T_VLANX, 0);
	lattr(&g, A_LONG_ALIAS, T_EMPTY, 0);
	emit("link", "read-long-alias", &g);

	g.n = 0;
	lread(&g, 7, 20, 10);
	lkernel(&g, 0, 1);
	lnewlink(&g, 1); lattr(&g, A_ALIAS, T_HOST, 0);
	emit("link", "read-alias-fills-buffer", &g);

	/* no name, no index, then a socket and a send that fail */
	g.n = 0;
	lread(&g, 255, 5, 5);
	lkernel(&g, 0, 0);
	lread(&g, 15, 5, 5);
	lkernel(&g, 0, 0);
	lread(&g, 8, 5, 5);
	lkernel(&g, 1, 0);
	lread(&g, 8, 5, 5);
	lkernel(&g, 2, 0);
	emit("link", "read-refused", &g);

	g.n = 0;
	put8(&g, 1); put8(&g, 15); put8(&g, T_SERVED);
	lkernel(&g, 0, 0);
	emit("link", "write-no-index", &g);

	g.n = 0;
	put8(&g, 1); put8(&g, 3); put8(&g, 0x80); put16(&g, 256);
	for (int i = 0; i < 256; i++)
		put8(&g, 'b');
	lkernel(&g, 0, 1);
	lack(&g);
	put8(&g, 1); put8(&g, 3); put8(&g, 0x80); put16(&g, 255);
	for (int i = 0; i < 255; i++)
		put8(&g, 'b');
	lkernel(&g, 0, 1);
	lack(&g);
	emit("link", "write-alias-256", &g);

	g.n = 0;
	put8(&g, 1); put8(&g, 3); put8(&g, T_SERVED);
	lkernel(&g, 0, 1);
	lerror(&g, 0);   /* -EPERM */
	emit("link", "write-refused", &g);

	g.n = 0;
	lmark(&g, 3);
	lkernel(&g, 0, 2);
	lnewlink(&g, 0);
	lack(&g);
	emit("link", "mark-plain-nic", &g);

	g.n = 0;
	lmark(&g, 3);
	lkernel(&g, 0, 2);
	lnewlink(&g, 0);
	lerror(&g, 0);
	emit("link", "mark-write-refused", &g);

	g.n = 0;
	lmark(&g, 4);
	lkernel(&g, 0, 1);
	lnewlink(&g, 2); lattr(&g, A_ALIAS, T_MINTED, 0); lattr(&g, A_KIND, T_VLAN, 0);
	lmark(&g, 5);
	lkernel(&g, 0, 1);
	lnewlink(&g, 1); lattr(&g, A_KIND, T_VLAN, 0);
	lmark(&g, 6);
	lkernel(&g, 0, 1);
	lnewlink(&g, 1); lattr(&g, A_ALIAS, T_HOST, 0);
	emit("link", "mark-minted-vlan", &g);

	g.n = 0;
	for (int k = 0; k < 2; k++) {
		lmark(&g, 3);
		lkernel(&g, 0, 2);
		lnewlink(&g, 0);
		lack(&g);
	}
	emit("link", "mark-twice", &g);

	g.n = 0;
	lmark(&g, 3);
	lkernel(&g, 0, 2);
	lnewlink(&g, 0);
	lack(&g);
	lmark(&g, 4);
	lkernel(&g, 0, 1);
	lnewlink(&g, 1); lattr(&g, A_ALIAS, T_SERVED, 0);
	put8(&g, 3);
	lkernel(&g, 0, 3);
	lnewlink(&g, 1); lattr(&g, A_ALIAS, T_HOST, 0);
	lnewlink(&g, 1); lattr(&g, A_ALIAS, T_SERVED, 0);
	lack(&g);
	emit("link", "unmark-after-host-renamed", &g);

	g.n = 0;
	for (unsigned k = 0, n = 0; n < 40; k++) {
		if (k % 16 == 15)
			continue;
		lmark(&g, k);
		lkernel(&g, 0, 2);
		lnewlink(&g, 0);
		lack(&g);
		n++;
	}
	emit("link", "mark-forty", &g);

	g.n = 0;
	{
		static const unsigned pairs[][2] = {
			{ T_EMPTY, T_SERVED }, { T_EMPTY, T_REACPW }, { T_VLA, T_EMPTY },
			{ T_VLANX, T_EMPTY }, { T_VLAN, T_EMPTY }, { T_EMPTY, T_HOST },
			{ T_VETH, T_EMPTY }, { T_EMPTY, T_PREFIX }, { T_VLAN, T_SERVED2 },
		};
		for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
			put8(&g, 4);
			put8(&g, pairs[i][0]);
			put8(&g, pairs[i][1]);
		}
	}
	emit("link", "decide-all", &g);

	g.n = 0;
	put8(&g, 5); put8(&g, 0);
	put8(&g, 5); put8(&g, 2);
	put8(&g, 5); put8(&g, 1);
	lmark(&g, 255);
	lkernel(&g, 0, 0);
	emit("link", "mark-refused", &g);

	/* a mark whose read fails, an unmark whose reads fail, a write answered with a link */
	g.n = 0;
	lmark(&g, 3);
	lkernel(&g, 1, 0);
	lmark(&g, 4);
	lkernel(&g, 0, 2);
	lnewlink(&g, 0);
	lack(&g);
	put8(&g, 3);
	lkernel(&g, 1, 0);
	put8(&g, 1); put8(&g, 5); put8(&g, T_HOST);
	lkernel(&g, 0, 1);
	lnewlink(&g, 1); lattr(&g, A_ALIAS, T_HOST, 0);
	emit("link", "reads-that-fail", &g);

	/* the kernel itself answers -ENODEV: a socket was opened, and that is no refusal */
	g.n = 0;
	lread(&g, 3, 5, 5);
	lkernel(&g, 0, 1);
	lerror(&g, 1);
	emit("link", "reply-says-enodev", &g);
}

/* ---- fuzz_segconf: [cap][flags][files] then per file [label][u16 len][text] ------------- */

/* The files of one seed; the last is the hand-written file, read last. */
static void segc(const char *name, unsigned cap, unsigned flags, int nfiles,
                 const unsigned *label, const char *const *text)
{
	struct g g = { .n = 0 };
	put8(&g, cap);
	put8(&g, flags);
	put8(&g, (unsigned)(nfiles - 1));
	for (int f = 0; f < nfiles; f++) {
		put8(&g, label[f]);
		put16(&g, (unsigned)strlen(text[f]));
		puts_(&g, text[f]);
	}
	emit("segconf", name, &g);
}

static void seg1(const char *name, unsigned cap, const char *text)
{
	const unsigned l[] = { 0 };
	const char *const t[] = { text };
	segc(name, cap, 0, 1, l, t);
}

static void gen_segconf(void)
{
	static char big[8192];
	size_t n;

	seg1("operator-file", 8,
	     "# ~/.config/reac-pw/reac-pw.conf\n"
	     "\n"
	     "[segment enp131s0.11]\n"
	     "role = tap          ; a switch mirror\n"
	     "\n"
	     "[segment enp131s0.12]\n"
	     "ignore = yes\n"
	     "\n"
	     "[segment enp131s0.1]\n"
	     "role = \"master\"\n"
	     "[segment enp131s0.2]\n"
	     "role = 'slave'\r\n");
	seg1("names-differ-in-case", 8, "[segment eth0]\nrole = tap\n[segment ETH0]\nrole = slave\n");
	n = 0;
	for (int i = 0; i < 33; i++)
		n += (size_t)snprintf(big + n, sizeof big - n, "[segment s%d]\nrole = tap\n", i);
	seg1("thirty-three-segments", 8, big);
	seg1("keyword-in-capitals", 8, "[SEGMENT eth0]\nrole = master\n[Segment eth1]\n");
	seg1("name-sixteen", 8, "[segment abcdefghijklmnop]\nrole = tap\n[segment abcdefghijklmno]\n");
	seg1("tab-after-keyword", 8, "[segment\teth0]\nrole = slave\n");
	seg1("text-after-header", 8, "[segment e0]\nrole = tap\n[segment e1] x\nrole = slave\n");
	seg1("role-in-capitals", 8, "[segment eth0]\nROLE = MASTER\n[segment eth1]\nRole = Box\n"
	     "Model = S1608\n");
	seg1("box-without-model", 8, "[segment eth0]\nrole = box\n[segment eth1]\nrole = box\n"
	     "model = s0808\n");
	n = (size_t)snprintf(big, sizeof big, "[segment e0]\nrole = tap\n[segment e1%*s]\n"
	                     "role = slave\n", 520, "x");
	seg1("long-header-line", 8, big);
	seg1("model-in-capitals", 8, "[segment e0]\nrole = box\nmodel = FR4000\n");
	seg1("model-on-a-tap", 8, "[segment e0]\nrole = tap\nmodel = s1608\n[segment e1]\n"
	     "model = s0808\nrole = box\n[segment e2]\nmodel = s9999\nrole = box\n");
	seg1("ignore-words", 8, "[segment a]\nignore = 1\n[segment b]\nignore = TRUE\n"
	     "[segment c]\nignore = yes\nignore = no\n[segment d]\nignore = 0\n[segment e]\n"
	     "ignore = maybe\n");
	/* the knock plan: the three plans of src/reac_knock_wire.h:88-93, in any case and
	 * without one pair of quotes, the last file to name one winning it; a segment with
	 * no `knock` line; and names of no plan, which are refused */
	seg1("knock-plans", 8, "[segment eth0]\nknock = control\n[segment eth1]\n"
	     "role = tap\nknock = escalate\n[segment eth2]\nrole = slave\n");
	{
		const unsigned l[] = { 51, 0 };
		const char *const t[] = { "[segment eth0]\nknock = \"STREAM\"\n",
		                          "[segment eth0]\nknock = Control\n[segment eth1]\n" };
		segc("knock-in-capitals", 8, 0, 2, l, t);
	}
	seg1("knock-refused", 8, "[segment eth0]\nknock = bogus\nknock =\n"
	     "[segment eth1]\nknock = controlx\n");
	{
		const unsigned l[] = { 50, 99, 0 };
		const char *const t[] = {
			"[segment eth0]\nrole = master\n[segment eth1]\nignore = yes\n",
			"[segment eth1]\nignore = no\n[segment eth3]\nrole = tap\n",
			"[segment eth0]\nrole = tap\n[segment eth2]\nrole = slave\n",
		};
		segc("dropin-overrides", 8, 0, 3, l, t);
	}
	{
		const unsigned l[] = { 0x80, 0 };
		const char *const t[] = { "[segment eth0]\nbogus = 1\nrole = master\n",
		                          "# the hand-written file\n" };
		segc("dropin-refusal", 8, 0, 2, l, t);
	}
	seg1("refusals-many", 8,
	     "role = master\n"
	     "[segment]\n"
	     "[nonsense eth0]\n"
	     "[segment eth0\n"
	     "[segment eth0]\n"
	     "just words\n"
	     "role = captain\n"
	     "model = s9999\n"
	     "ignore = perhaps\n"
	     "colour = red\n"
	     "[segment eth1]\n"
	     "role = box\n");
	seg1("header-twice-in-one-file", 8, "[segment eth0]\nrole = tap\n[segment eth0]\nignore = yes\n");
	seg1("absent-probes", 8, "[segment eth0]\nrole = tap\n");
	seg1("vlans", 8,
	     "[segment eth0.33]\n"
	     "[segment eth1.33]\n"
	     "[segment eth0.4095]\n"
	     "[segment eth0.4094]\n"
	     "[segment eth0.1a]\n"
	     "[segment eth2.7]\nignore = yes\n"
	     "[segment br@x.5]\n"
	     "[segment .5]\n"
	     "[segment eth0.]\n"
	     "[segment eth0.0]\n"
	     "[segment eth0.33]\n");
	{
		const unsigned l[] = { 0 };
		const char *const t[] = { "[segment enp1s0.33]\n[segment eth0.1]\n[segment eth0.2]\n"
		                          "[segment eth0.3]\n" };
		segc("vlans-full", 2, 1, 1, l, t);
	}
	seg1("empty", 0, "");
	seg1("comments-only", 3, "# nothing\n; nothing\n   \n\t# indented\nx#y = 1\n");
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: gen-corpus <directory>\n");
		return 2;
	}
	g_dir = argv[1];
	if (mkdir(g_dir, 0755) != 0 && errno != EEXIST) {
		perror(g_dir);
		return 1;
	}
	gen_pin();
	gen_headamp();
	gen_rate();
	gen_role();
	gen_qdisc();
	gen_link();
	gen_segconf();
	return g_failed;
}
