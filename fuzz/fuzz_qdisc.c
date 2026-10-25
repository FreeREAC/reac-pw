// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libFuzzer harness: THE etf QDISC COUNTERS (src/reac_qdisc.c, reac_qdisc_stats_read), the
 * one RTM_GETQDISC dump the daemon reads every health window, and the catch-up budget it
 * derives from the pacer's lead (reac_qdisc_etf_catchup_slots).
 *
 * Input:  [0] reads, mod 4; then each read:
 *           [ifindex i8] [flags: bit 0 socket() fails, bit 1 send() fails, bit 2 a NULL
 *           out] [errno byte: socket/send] [errno byte: recv's once the replies end]
 *           [chunks, mod 5] each chunk [mode]: mode & 1 the input's own bytes
 *           (netlink.h), else [messages, mod 8] each spelled by msg_build;
 *         then [u16 lead_us][u8 fps selector] for the catch-up budget.
 *
 * The expectations come from the bytes the fake kernel hands out, walked by the netlink
 * grammar of fuzz/netlink.h and the rule the header states (src/reac_qdisc.h:84-120):
 * only RTM_NEWQDISC messages of the device asked for whose kind is `etf` are summed,
 * from TCA_STATS2 when the message has one, else from TCA_STATS; a dump that ends in
 * NLMSG_DONE answers 0 and the sums; one that cannot be made answers -errno and leaves
 * the caller's counters as they were.
 *
 * Properties, each an abort():
 *   - the reader closes the socket it opened, and uses no other descriptor;
 *   - it asks for one dump of the qdiscs of the device it was given;
 *   - a completed dump answers 0 and exactly the sums of the device's etf qdiscs;
 *   - a dump that fails answers its error and leaves the counters untouched;
 *   - a NULL out or a device index of 0 or less is -EINVAL and opens nothing;
 *   - and the catch-up budget is the lead past the qdisc's delta, in slots, from 1 to
 *     1000000. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "reac_qdisc.h"

#include "fuzz.h"
#include "netlink.h"

/* reac_qdisc.c's policy half (arm, disarm, release) calls libreac-transport, which no
 * harness links. This harness never calls that half; these stand-ins only let the
 * reader's file link, and answer as a host with no qdisc support would. */
#include <reac/transport/reac_etf_qdisc.h>
enum reac_etf_qdisc_state reac_etf_qdisc_state(int ifindex, char *root_kind, size_t cap)
{
	(void)ifindex;
	if (root_kind && cap)
		root_kind[0] = '\0';
	return REAC_ETF_QDISC_UNREADABLE;
}

int reac_etf_qdisc_install(int ifindex, uint32_t delta_ns)
{
	(void)ifindex;
	(void)delta_ns;
	return -EOPNOTSUPP;
}

int reac_etf_qdisc_remove(int ifindex)
{
	(void)ifindex;
	return -EOPNOTSUPP;
}

const char *reac_etf_qdisc_fix(int err)
{
	(void)err;
	return "";
}

/* contract: linux/rtnetlink.h (RTM_NEWQDISC 36, RTM_GETQDISC 38, RTM_NEWLINK 16) and
 * linux/netlink.h (NLM_F_REQUEST 1, NLM_F_DUMP 0x300) */
#define RTM_NEWQDISC_ 36
#define RTM_GETQDISC_ 38
#define RTM_NEWLINK_  16
#define F_REQUEST     0x1
#define F_DUMP        0x300
/* contract: linux/rtnetlink.h (struct tcmsg: family, two pads, int ifindex, then handle,
 * parent, info: 20 bytes; tcm_ifindex at 4) and linux/pkt_sched.h, linux/gen_stats.h
 * (TCA_KIND 1, TCA_STATS 3, TCA_STATS2 7; TCA_STATS_BASIC 1, TCA_STATS_QUEUE 3) */
#define TCMSG_LEN     20
#define TCA_KIND_     1
#define TCA_STATS_    3
#define TCA_STATS2_   7
#define ST_BASIC      1
#define ST_QUEUE      3
/* contract: linux/gen_stats.h (struct gnet_stats_basic: u64 bytes, u32 packets, 16 bytes
 * on an LP64 ABI; struct gnet_stats_queue: qlen, backlog, drops, requeues, overlimits,
 * five u32) and linux/pkt_sched.h (struct tc_stats: u64 bytes, then packets, drops,
 * overlimits, bps, pps, qlen, backlog, u32 each: 40 bytes on an LP64 ABI) */
#define BASIC_LEN     16
#define QUEUE_LEN     20
#define TC_STATS_LEN  40
/* contract: libreac 1.8.0 include/reac/transport/reac_etf_qdisc.h:42 (the qdisc's delta,
 * 300000 ns) */
#define ETF_DELTA_US  300u

#define READS_MAX 3

/* ---- the replies the input spells --------------------------------------------------- */

/* One message of a dump. [kind] mod 8. */
static void msg_build(struct fuzz_in *in, struct nl_w *w, int ifindex)
{
	static const char *const kinds[] = { "fq_codel", "etf", "", "etfx", "et" };
	uint8_t kind = fuzz_u8(in);
	switch (kind % 8) {
	case 0: case 6: {
		/* a qdisc: of the device asked for, or another; then attributes */
		size_t at = nl_begin(w, RTM_NEWQDISC_);
		uint8_t tcm[TCMSG_LEN] = { 0 };
		int32_t idx = (fuzz_u8(in) & 1) ? ifindex + 1 : ifindex;
		memcpy(tcm + 4, &idx, 4);
		nl_put(w, tcm, sizeof tcm);
		int nattr = fuzz_u8(in) % 6;
		for (int i = 0; i < nattr; i++) {
			uint8_t a = fuzz_u8(in);
			switch (a % 6) {
			case 0:
				nl_attr_put(w, TCA_KIND_, "etf", 4);
				break;
			case 1: {
				/* another kind; a & 0x80 leaves its NUL off */
				const char *k = kinds[(a >> 3) % 5];
				nl_attr_put(w, TCA_KIND_, k, strlen(k) + !(a & 0x80));
				break;
			}
			case 2: {
				size_t n = nl_nest(w, TCA_STATS2_);
				int nsub = fuzz_u8(in) % 4;
				for (int k = 0; k < nsub; k++) {
					uint8_t s = fuzz_u8(in);
					uint8_t b[QUEUE_LEN] = { 0 };
					uint64_t v64 = fuzz_u32(in);
					uint32_t v1 = fuzz_u32(in), v2 = fuzz_u32(in);
					if (s % 3 == 0) {
						memcpy(b, &v64, 8);
						memcpy(b + 8, &v1, 4);
						/* s & 0x80: a payload one byte short */
						nl_attr_put(w, ST_BASIC, b, BASIC_LEN - !!(s & 0x80));
					} else if (s % 3 == 1) {
						memcpy(b + 8, &v1, 4);
						memcpy(b + 16, &v2, 4);
						nl_attr_put(w, ST_QUEUE, b, QUEUE_LEN - !!(s & 0x80));
					} else {
						nl_attr_put(w, 99, b, 4);
					}
				}
				nl_nest_end(w, n);
				break;
			}
			case 3: case 4: {
				uint8_t b[TC_STATS_LEN] = { 0 };
				uint64_t v64 = fuzz_u32(in);
				uint32_t v[3] = { fuzz_u32(in), fuzz_u32(in), fuzz_u32(in) };
				memcpy(b, &v64, 8);
				memcpy(b + 8, v, 12);
				/* 4: a TCA_STATS too short for its struct */
				nl_attr_put(w, TCA_STATS_, b, a % 6 == 3 ? TC_STATS_LEN : 20);
				break;
			}
			default:
				nl_attr_put(w, 99, "abcd", 4);
				break;
			}
		}
		nl_end(w, at);
		/* 6: the message's length is not padded, and the chunk may end there */
		if (kind % 8 == 6 && w->n == NL_ALIGN(w->n)) {
			nl_put(w, "", 1);
			nl_end(w, at);
		} else {
			nl_pad(w);
		}
		break;
	}
	case 1: {
		size_t at = nl_begin(w, NL_DONE);
		nl_put32(w, 0);
		nl_end(w, at);
		break;
	}
	case 2: {
		static const int32_t codes[] = { 0, -EPERM, -ENODEV, -EOPNOTSUPP, 5 };
		uint8_t c = fuzz_u8(in);
		nl_error(w, codes[c % 5], (c & 0x80) ? (c >> 4 & 7) : -1);
		break;
	}
	case 3: {
		/* a qdisc message too short for its tcmsg */
		size_t at = nl_begin(w, RTM_NEWQDISC_);
		nl_put32(w, 0);
		nl_put32(w, (uint32_t)ifindex);
		nl_end(w, at);
		break;
	}
	case 4: {
		size_t at = nl_begin(w, RTM_NEWLINK_);
		nl_put(w, "0123456789abcdef", 16);
		nl_end(w, at);
		break;
	}
	case 5: {
		/* a header whose length is shorter than a header */
		nl_put32(w, 8);
		nl_put32(w, NL_DONE);
		break;
	}
	default: {
		/* a header whose length runs past the chunk */
		size_t at = nl_begin(w, NL_DONE);
		nl_set32(w, at, 4000);
		break;
	}
	}
}

static void queue_chunks(struct fuzz_in *in, int ifindex)
{
	int n = fuzz_u8(in) % 5;
	for (int c = 0; c < n; c++) {
		uint8_t mode = fuzz_u8(in);
		if (mode & 1) {
			nl_queue_raw(in);
			continue;
		}
		struct nl_w w = { .n = 0 };
		int m = fuzz_u8(in) % 8;
		for (int k = 0; k < m && !w.full; k++)
			msg_build(in, &w, ifindex);
		if (w.n == 0)
			nl_put32(&w, 0);   /* a chunk is never empty */
		nl_queue(&w);
	}
}

/* ---- what the reader owes ------------------------------------------------------------ */

struct expect {
	int rc;               /* 0, an error, or 1 for "an error the kernel cannot send" */
	struct reac_qdisc_stats sum;
};

static uint64_t rd64(const uint8_t *p)
{
	uint64_t v;
	memcpy(&v, p, 8);
	return v;
}

/* The counters of one RTM_NEWQDISC message at c + off, `len` bytes. */
static void take_qdisc(const uint8_t *c, long off, uint32_t len, int ifindex,
                       struct reac_qdisc_stats *s)
{
	if (len < NL_HDR + TCMSG_LEN)
		return;
	if ((int32_t)nl_rd32(c + off + NL_HDR + 4) != ifindex)
		return;
	long a = off + NL_HDR + NL_ALIGN(TCMSG_LEN), left = (long)len - NL_HDR - TCMSG_LEN;
	long kind = -1, st2 = -1, st1 = -1;
	uint16_t t, l;
	while ((l = nl_attr(c, &a, &left, &t))) {
		if (t == TCA_KIND_)
			kind = a;
		else if (t == TCA_STATS2_)
			st2 = a;
		else if (t == TCA_STATS_)
			st1 = a;
		nl_attr_next(l, &a, &left);
	}
	if (kind < 0)
		return;
	size_t kl = nl_rd16(c + kind) - RTA_HDR;
	const char *k = (const char *)c + kind + RTA_HDR;
	/* the kind is "etf", NUL-terminated inside its payload */
	if (kl < 4 || memcmp(k, "etf", 4) != 0)
		return;
	s->qdiscs++;
	if (st2 >= 0) {
		long b = st2 + RTA_HDR, bl = (long)nl_rd16(c + st2) - RTA_HDR;
		while ((l = nl_attr(c, &b, &bl, &t))) {
			const uint8_t *d = c + b + RTA_HDR;
			if (t == ST_BASIC && l - RTA_HDR >= BASIC_LEN) {
				s->bytes += rd64(d);
				s->packets += nl_rd32(d + 8);
			} else if (t == ST_QUEUE && l - RTA_HDR >= QUEUE_LEN) {
				s->drops += nl_rd32(d + 8);
				s->overlimits += nl_rd32(d + 16);
			}
			nl_attr_next(l, &b, &bl);
		}
	} else if (st1 >= 0 && nl_rd16(c + st1) - RTA_HDR >= TC_STATS_LEN) {
		const uint8_t *d = c + st1 + RTA_HDR;
		s->bytes += rd64(d);
		s->packets += nl_rd32(d + 8);
		s->drops += nl_rd32(d + 12);
		s->overlimits += nl_rd32(d + 16);
	}
}

/* The dump from the chunk `first` on, and its answer. */
static void expect_dump(int first, int ifindex, struct expect *x)
{
	memset(x, 0, sizeof *x);
	for (int c = first;; c++) {
		if (c >= g_nl.n_chunks) {
			x->rc = -g_nl.end_errno;
			return;
		}
		const uint8_t *b = g_nl.chunk[c];
		long off = 0, left = (long)g_nl.chunk_len[c];
		uint16_t type;
		uint32_t len;
		while ((len = nl_msg(b, &off, &left, &type))) {
			if (type == NL_DONE) {
				x->rc = 0;
				return;
			}
			if (type == NL_ERROR) {
				int32_t e = len < NL_HDR + NL_ERR_BODY ? -EIO : (int32_t)nl_rd32(b + off + NL_HDR);
				/* netlink(7): an error is a negative errno, or 0 for an ACK, which
				 * answers no dump; a positive one is no reply the kernel sends, and
				 * the harness claims only that it is not a success */
				x->rc = e == 0 ? -EIO : e > 0 ? 1 : e;
				return;
			}
			if (type == RTM_NEWQDISC_)
				take_qdisc(b, off, len, ifindex, &x->sum);
			nl_msg_next(len, &off, &left);
		}
	}
}

/* The request a read owes: a 36-byte message, RTM_GETQDISC, a dump request, sequence 1,
 * then a tcmsg of family AF_UNSPEC (0) naming the device. */
static int request_is(const uint8_t *m, size_t len, int ifindex)
{
	uint8_t want[NL_HDR + TCMSG_LEN] = { 0 };
	uint32_t l = sizeof want, seq = 1;
	uint16_t type = RTM_GETQDISC_, flags = F_REQUEST | F_DUMP;
	int32_t idx = ifindex;
	memcpy(want, &l, 4);
	memcpy(want + 4, &type, 2);
	memcpy(want + 6, &flags, 2);
	memcpy(want + 8, &seq, 4);
	memcpy(want + NL_HDR + 4, &idx, 4);
	return len == sizeof want && memcmp(m, want, sizeof want) == 0;
}

/* The counters as the harness filled them before the read. */
static int untouched(const struct reac_qdisc_stats *out)
{
	const uint8_t *p = (const uint8_t *)out;
	for (size_t i = 0; i < sizeof *out; i++)
		if (p[i] != 0x5a)
			return 0;
	return 1;
}

/* The catch-up budget the header states. */
static int slots_of(unsigned lead_us, int fps)
{
	if (fps <= 0)
		return 1;
	unsigned long long usable = lead_us > ETF_DELTA_US ? lead_us - ETF_DELTA_US : 0;
	unsigned long long n = usable * (unsigned long long)fps / 1000000ull;
	return n < 1 ? 1 : n > 1000000ull ? 1000000 : (int)n;
}

/* ---- the checks ---------------------------------------------------------------------- */

static FUZZ_NOINLINE void check_socket(const struct nl_kernel *k)
{
	/* Every socket the reader opens it closes, before it returns, and it sends, reads and
	 * closes only that socket.
	 * contract: src/reac_qdisc.h:118-120 (one RTM_GETQDISC dump per read)
	 * mutant: src/reac_qdisc.c:317 `close(fd);` -> ``
	 * input: two-etf-qdiscs
	 * mutant: src/reac_qdisc.c:279 `close(fd);` -> ``
	 * input: send-fails */
	if (k->open || k->opened != k->closed || k->misuse)
		abort();
}

static FUZZ_NOINLINE void check_request(const uint8_t *m, size_t len, int ifindex)
{
	/* The one request is a dump of the qdiscs (RTM_GETQDISC, NLM_F_REQUEST | NLM_F_DUMP)
	 * of the device asked for, a header and a tcmsg.
	 * contract: src/reac_qdisc.h:101-105 (one RTM_GETQDISC dump), linux/rtnetlink.h
	 * mutant: src/reac_qdisc.c:272 `NLM_F_REQUEST | NLM_F_DUMP` -> `NLM_F_REQUEST`
	 * input: two-etf-qdiscs
	 * mutant: src/reac_qdisc.c:275 `= ifindex;` -> `= 0;`
	 * input: two-etf-qdiscs */
	if (!request_is(m, len, ifindex))
		abort();
}

static FUZZ_NOINLINE void check_sums(int rc, const struct reac_qdisc_stats *out,
                                     const struct reac_qdisc_stats *want)
{
	/* A dump that reached NLMSG_DONE answers 0, and the counters are the sums over the
	 * etf qdiscs of the device: its TCA_STATS2 basic and queue blocks when it has one,
	 * else its TCA_STATS; another device's qdiscs, another kind and a kind that is not
	 * NUL-terminated count nothing.
	 * contract: src/reac_qdisc.h:110-120, src/reac_qdisc.c:171-186
	 * mutant: src/reac_qdisc.c:232 `tcm->tcm_ifindex != sum->ifindex` -> `0`
	 * input: other-device
	 * mutant: src/reac_qdisc.c:246 `strcmp(name, "etf") != 0` -> `strncmp(name, "etf", 2) != 0`
	 * input: other-kinds
	 * mutant: src/reac_qdisc.c:250 `if (st2)` -> `if (0)`
	 * input: two-etf-qdiscs
	 * mutant: src/reac_qdisc.c:207 `o->drops      += q.drops;` -> `o->drops      = q.drops;`
	 * input: two-etf-qdiscs
	 * mutant: src/reac_qdisc.c:217 `RTA_PAYLOAD(rta) < sizeof(struct tc_stats)` -> `RTA_PAYLOAD(rta) < 20`
	 * input: stats1-short
	 * mutant: src/reac_qdisc.c:223 `o->drops      += s.drops;` -> `o->drops      += s.overlimits;`
	 * input: stats1-only
	 * mutant: src/reac_qdisc.c:301 `{ done = 1; break; }` -> `continue;`
	 * input: done-then-more
	 * mutant: src/reac_qdisc.c:243 `RTA_PAYLOAD(kind) == 0` -> `RTA_PAYLOAD(kind) == 4`
	 * input: two-etf-qdiscs */
	if (rc != 0 || memcmp(out, want, sizeof *out) != 0)
		abort();
}

static FUZZ_NOINLINE void check_failed(int rc, int want, const struct reac_qdisc_stats *out)
{
	/* A dump that could not be made answers its error, never 0, and leaves the caller's
	 * counters as they were: unreadable is not zero. An error message too short for its
	 * code is -EIO.
	 * contract: src/reac_qdisc.h:107-109, 118-120
	 * mutant: src/reac_qdisc.c:309 `err->error ? err->error : -EIO` -> `err->error`
	 * input: error-ack
	 * mutant: src/reac_qdisc.c:306 `nh->nlmsg_len < NLMSG_LENGTH(sizeof *err)` -> `0`
	 * input: error-short
	 * mutant: src/reac_qdisc.c:292 `rc = -errno;` -> `rc = 0;`
	 * input: replies-end
	 * mutant: src/reac_qdisc.c:263 `return -errno;` -> `return 0;`
	 * input: socket-fails
	 * mutant: src/reac_qdisc.c:300 `NLMSG_OK(nh, n)` -> `NLMSG_OK(nh, (unsigned)n)`
	 * input: unpadded-last */
	if (rc == 0 || (want != 1 && rc != want) || !untouched(out))
		abort();
}

static FUZZ_NOINLINE void check_einval(int rc, int opened_before, int opened)
{
	/* A NULL out, or an index no device has, is -EINVAL, and nothing is asked of the
	 * kernel.
	 * contract: src/reac_qdisc.c:258-259
	 * mutant: src/reac_qdisc.c:258 `ifindex <= 0` -> `ifindex < 0`
	 * input: index-zero */
	if (rc != -EINVAL || opened != opened_before)
		abort();
}

static FUZZ_NOINLINE void check_slots(int got, unsigned lead_us, int fps)
{
	/* The catch-up budget is the lead past the qdisc's delta, in slots at fps, rounded
	 * down, at least 1 and at most 1000000; no frame rate is 1.
	 * contract: src/reac_qdisc.h:122-143
	 * mutant: src/reac_qdisc.c:329 `lead_us - delta_us` -> `lead_us`
	 * input: two-etf-qdiscs
	 * mutant: src/reac_qdisc.c:334 `if (n < 1)` -> `if (n < 0)`
	 * input: index-zero */
	if (got != slots_of(lead_us, fps))
		abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size >= FUZZ_INPUT_MAX)
		return 0;
	struct fuzz_in in = fuzz_in(data, size);
	memset(&g_nl, 0, sizeof g_nl);
	int reads = fuzz_u8(&in) % (READS_MAX + 1);
	for (int r = 0; r < reads; r++) {
		int ifindex = (int8_t)fuzz_u8(&in);
		uint8_t flags = fuzz_u8(&in);
		int call_errno = nl_errno(fuzz_u8(&in));
		g_nl.end_errno = nl_errno(fuzz_u8(&in));
		g_nl.socket_errno = (flags & 1) ? call_errno : 0;
		g_nl.send_errno = (flags & 2) ? call_errno : 0;
		/* each read has its own replies, after every chunk an earlier one queued */
		int first = g_nl.n_chunks;
		queue_chunks(&in, ifindex);
		g_nl.next = first;
		int sent_before = g_nl.n_sent, opened_before = g_nl.opened;

		struct expect x;
		expect_dump(first, ifindex, &x);
		struct reac_qdisc_stats out;
		memset(&out, 0x5a, sizeof out);
		int rc = reac_qdisc_stats_read(ifindex, (flags & 4) ? NULL : &out);
		check_socket(&g_nl);
		if ((flags & 4) || ifindex <= 0) {
			check_einval(rc, opened_before, g_nl.opened);
		} else if (flags & 1) {
			check_failed(rc, -call_errno, &out);
		} else {
			if (flags & 2) {
				check_failed(rc, -call_errno, &out);
				continue;
			}
			check_request(g_nl.sent[sent_before], g_nl.sent_len[sent_before], ifindex);
			if (x.rc == 0)
				check_sums(rc, &out, &x.sum);
			else
				check_failed(rc, x.rc, &out);
		}
	}
	unsigned lead_us = fuzz_u16(&in);
	static const int fps[] = { 8000, 4000, 0, -1, 96000, 2147483647 };
	int f = fps[fuzz_u8(&in) % 6];
	check_slots(reac_qdisc_etf_catchup_slots(lead_us, f), lead_us, f);
	return 0;
}
