// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Unit test for reac_topo — the tag classifier and the per-parent VLAN table, and for
 * reac_vlan_name, the one pure function of the netdev half.
 *
 * The classifier is where a trunk becomes visible at all, and it is fed the two shapes a
 * kernel can hand it, because which one arrives is a driver's business:
 *   (a) the ACCELERATED tag — the buffer holds no 802.1Q header and the VID is in
 *       PACKET_AUXDATA, which is what this kernel does (measured 2026-09-09,
 *       7.2.4-200.fc44, openmixer's tools/probe-vlan-8819.py: `802.1Q header ABSENT from
 *       the buffer`, `PACKET_AUXDATA vlan_tci = 111`);
 *   (b) the IN-BUFFER tag — 0x8100 at offset 12 and the real ethertype behind it;
 *   (c) QinQ, where the kernel strips the outer tag and leaves the inner one in the bytes:
 *       the OUTER VID names the netdev the frame arrives on, so the accelerated tag wins;
 *   (d) VID 0 — a priority tag names no VLAN, and `<parent>.0` must never be minted.
 *
 * The table's job is the lifecycle §4d states line by line: a fresh VID asks to be ensured
 * ONCE, an adopted netdev is never released with `minted`, a minted one always is, silence
 * past the hold releases, and a failed ensure retries on a window instead of at wire speed.
 */
#include <reac/transport/reac_topo.h>
#include <reac/transport/reac_vlan.h>

#include <stdio.h>
#include <string.h>

#define CHK(cond) do { if (!(cond)) { \
	fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__); return 1; } } while (0)

#define SEC 1000000000ULL

/* A frame as a tap reads it: dst, src, then whatever ethertypes are asked for. */
static size_t frame(uint8_t *b, const uint16_t *ets, int n)
{
	memset(b, 0, 64);
	memcpy(b, "\x00\x40\xab\xc9\xcc\x03\x00\x40\xab\xc4\x80\x41", 12);
	size_t off = 12;
	for (int i = 0; i < n; i++) {
		b[off] = (uint8_t)(ets[i] >> 8);
		b[off + 1] = (uint8_t)(ets[i] & 0xff);
		off += 2;
	}
	return 64;
}

int main(void)
{
	uint8_t b[64];
	uint16_t vid = 0xffff;

	/* ---- (a) the accelerated tag: no header in the bytes, the VID from auxdata. This is
	 * the shape this kernel produces and the one the daemon will meet on the rig. */
	uint16_t plain[] = { 0x8819 };
	size_t n = frame(b, plain, 1);
	CHK(reac_topo_classify(b, n, 1, 111, &vid) == REAC_TOPO_TAGGED);
	CHK(vid == 111);
	/* The same frame with the priority bits set: only the low 12 bits are the VID. */
	CHK(reac_topo_classify(b, n, 1, 0xe000 | 4094, &vid) == REAC_TOPO_TAGGED);
	CHK(vid == 4094);

	/* ---- and the SAME BYTES with no valid tag are the untagged case. This is fact B of
	 * the spec's §3 measured table seen from the classifier's side: the parent's own copy
	 * of a tagged frame is byte-identical to an untagged one, so ONLY the auxdata bit
	 * separates them. A classifier that read the buffer alone would call every trunk an
	 * access port. */
	CHK(reac_topo_classify(b, n, 0, 0, &vid) == REAC_TOPO_UNTAGGED);
	CHK(vid == 0);

	/* ---- (b) the in-buffer tag, for a driver that does not strip it. */
	uint16_t ctag[] = { 0x8100, 12, 0x8819 };
	n = frame(b, ctag, 3);
	CHK(reac_topo_classify(b, n, 0, 0, &vid) == REAC_TOPO_TAGGED);
	CHK(vid == 12);
	uint16_t stag[] = { 0x88a8, 3001, 0x8819 };
	n = frame(b, stag, 3);
	CHK(reac_topo_classify(b, n, 0, 0, &vid) == REAC_TOPO_TAGGED);
	CHK(vid == 3001);

	/* ---- (c) QinQ: outer stripped into metadata, inner left in the bytes. The netdev a
	 * frame arrives on is named by the OUTER tag, so 11 wins over 12. */
	uint16_t inner[] = { 0x8100, 12, 0x8819 };
	n = frame(b, inner, 3);
	CHK(reac_topo_classify(b, n, 1, 11, &vid) == REAC_TOPO_TAGGED);
	CHK(vid == 11);

	/* ---- (d) VID 0 is a priority tag and names no VLAN. */
	n = frame(b, plain, 1);
	CHK(reac_topo_classify(b, n, 1, 0xe000, &vid) == REAC_TOPO_UNTAGGED);
	CHK(vid == 0);
	uint16_t ctag0[] = { 0x8100, 0, 0x8819 };
	n = frame(b, ctag0, 3);
	CHK(reac_topo_classify(b, n, 0, 0, &vid) == REAC_TOPO_UNTAGGED);

	/* ---- anything that is not REAC is refused, tagged or not, and so is a runt. The BPF
	 * should have dropped these; the classifier does not rely on it. */
	uint16_t ip[] = { 0x0800 };
	n = frame(b, ip, 1);
	CHK(reac_topo_classify(b, n, 1, 111, &vid) == REAC_TOPO_NOT_REAC);
	uint16_t tagged_ip[] = { 0x8100, 11, 0x0800 };
	n = frame(b, tagged_ip, 3);
	CHK(reac_topo_classify(b, n, 0, 0, &vid) == REAC_TOPO_NOT_REAC);
	CHK(reac_topo_classify(b, 13, 1, 111, &vid) == REAC_TOPO_NOT_REAC);
	CHK(reac_topo_classify(NULL, 64, 1, 111, &vid) == REAC_TOPO_NOT_REAC);

	/* ---- the name the netdev takes, and the truncation it refuses. */
	char name[IFNAMSIZ];
	CHK(reac_vlan_name("enp131s0", 11, name, sizeof name) == 0);
	CHK(strcmp(name, "enp131s0.11") == 0);
	CHK(reac_vlan_name("enp131s0", 4094, name, sizeof name) == 0);
	CHK(strcmp(name, "enp131s0.4094") == 0);
	/* IFNAMSIZ is 16 with the NUL, so a 12-character NIC plus ".4094" does not fit. A
	 * truncated interface name is a DIFFERENT interface — refuse, never trim. */
	CHK(reac_vlan_name("enp128s20f0u2", 4094, name, sizeof name) == -1);
	CHK(name[0] == '\0');

	/* ---- the table: a fresh VID asks to be ensured exactly once. */
	struct reac_topo t;
	struct reac_topo_event ev;
	reac_topo_init(&t);
	CHK(reac_topo_watch(&t, "trunk0") == 0);
	CHK(reac_topo_watch(&t, "trunk0") == 0);       /* idempotent */
	CHK(reac_topo_is_trunk(&t, "trunk0") == 0);    /* silence is never a topology */
	CHK(reac_topo_is_trunk(&t, "nosuch") == 0);

	reac_topo_saw(&t, "trunk0", REAC_TOPO_TAGGED, 11, 1 * SEC);
	reac_topo_saw(&t, "trunk0", REAC_TOPO_TAGGED, 11, 1 * SEC + 1);
	reac_topo_saw(&t, "trunk0", REAC_TOPO_TAGGED, 12, 1 * SEC + 2);
	CHK(reac_topo_next(&t, &ev) == 1);
	CHK(ev.verb == REAC_TOPO_ENSURE && strcmp(ev.parent, "trunk0") == 0 && ev.vid == 11);
	CHK(reac_topo_next(&t, &ev) == 1);
	CHK(ev.verb == REAC_TOPO_ENSURE && ev.vid == 12);
	CHK(reac_topo_next(&t, &ev) == 0);
	CHK(reac_topo_is_trunk(&t, "trunk0") == 1);
	CHK(reac_topo_count(&t, "trunk0", REAC_TOPO_VLAN_HEARD) == 2);
	CHK(reac_topo_vlan_find(&t, "trunk0", 11)->frames == 2);

	/* ---- untagged REAC on a trunk is refused BY NAME, and said once (§4f). */
	CHK(reac_topo_untagged_on_trunk(&t, "trunk0") == 0);   /* nothing untagged yet */
	reac_topo_saw(&t, "trunk0", REAC_TOPO_UNTAGGED, 0, 2 * SEC);
	CHK(reac_topo_untagged_on_trunk(&t, "trunk0") == 1);
	CHK(reac_topo_untagged_on_trunk(&t, "trunk0") == 0);   /* not once per frame */

	/* ---- one was CREATED, one was ADOPTED, and the difference survives to the release. */
	reac_topo_ensured(&t, "trunk0", 11, 1);
	reac_topo_ensured(&t, "trunk0", 12, 0);
	CHK(reac_topo_count(&t, "trunk0", REAC_TOPO_VLAN_SERVED) == 2);
	CHK(reac_topo_vlan_find(&t, "trunk0", 11)->minted == 1);
	CHK(reac_topo_vlan_find(&t, "trunk0", 12)->minted == 0);

	/* ---- a served VID does not ask again, however many frames it carries. */
	reac_topo_saw(&t, "trunk0", REAC_TOPO_TAGGED, 11, 3 * SEC);
	reac_topo_saw(&t, "trunk0", REAC_TOPO_TAGGED, 12, 3 * SEC);
	CHK(reac_topo_next(&t, &ev) == 0);

	/* ---- silence past the hold releases, and nothing before it does. VID 12 keeps
	 * speaking and must be untouched by VID 11's silence. */
	reac_topo_tick(&t, 3 * SEC + REAC_TOPO_SILENCE_HOLD_NS - 1);
	CHK(reac_topo_next(&t, &ev) == 0);
	reac_topo_saw(&t, "trunk0", REAC_TOPO_TAGGED, 12, 3 * SEC + REAC_TOPO_SILENCE_HOLD_NS);
	reac_topo_tick(&t, 3 * SEC + REAC_TOPO_SILENCE_HOLD_NS);
	CHK(reac_topo_next(&t, &ev) == 1);
	CHK(ev.verb == REAC_TOPO_RELEASE && ev.vid == 11 && ev.minted == 1);
	CHK(reac_topo_next(&t, &ev) == 0);
	CHK(reac_topo_vlan_find(&t, "trunk0", 11) == NULL);
	CHK(reac_topo_count(&t, "trunk0", REAC_TOPO_VLAN_SERVED) == 1);

	/* ---- and the same VID heard again is ensured again, from nothing. */
	reac_topo_saw(&t, "trunk0", REAC_TOPO_TAGGED, 11, 100 * SEC);
	CHK(reac_topo_next(&t, &ev) == 1);
	CHK(ev.verb == REAC_TOPO_ENSURE && ev.vid == 11);

	/* ---- a FAILED ensure (no CAP_NET_ADMIN) retries on a window, not per frame. This is
	 * the "report, never fail deaf" half: the daemon goes on hearing the VID it cannot
	 * serve, and asks again later in case the capability arrived with a restart. */
	reac_topo_ensure_failed(&t, "trunk0", 11, 100 * SEC);
	for (uint64_t k = 1; k < 100; k++)
		reac_topo_saw(&t, "trunk0", REAC_TOPO_TAGGED, 11, 100 * SEC + k);
	CHK(reac_topo_next(&t, &ev) == 0);
	reac_topo_saw(&t, "trunk0", REAC_TOPO_TAGGED, 11, 100 * SEC + REAC_TOPO_RETRY_NS);
	CHK(reac_topo_next(&t, &ev) == 1);
	CHK(ev.verb == REAC_TOPO_ENSURE && ev.vid == 11);
	reac_topo_ensured(&t, "trunk0", 11, 1);

	/* ---- the parent goes away: everything we minted under it is released, what we
	 * adopted is only forgotten. A netdev is released with the truth about who made it,
	 * because that is the only thing that decides whether it is deleted. */
	reac_topo_unwatch(&t, "trunk0", 200 * SEC);
	int rel_minted = 0, rel_adopted = 0;
	while (reac_topo_next(&t, &ev)) {
		CHK(ev.verb == REAC_TOPO_RELEASE);
		if (ev.minted)
			rel_minted++;
		else
			rel_adopted++;
	}
	CHK(rel_minted == 1);    /* vid 11, minted */
	CHK(rel_adopted == 1);   /* vid 12, adopted */
	CHK(reac_topo_find(&t, "trunk0") == NULL);

	/* ---- the clean exit releases every netdev still held, in one call. */
	reac_topo_init(&t);
	CHK(reac_topo_watch(&t, "p0") == 0);
	CHK(reac_topo_watch(&t, "p1") == 0);
	reac_topo_saw(&t, "p0", REAC_TOPO_TAGGED, 11, SEC);
	reac_topo_saw(&t, "p1", REAC_TOPO_TAGGED, 11, SEC);
	while (reac_topo_next(&t, &ev)) { }
	reac_topo_ensured(&t, "p0", 11, 1);
	reac_topo_ensured(&t, "p1", 11, 0);
	reac_topo_release_all(&t);
	int n_rel = 0, n_del = 0;
	while (reac_topo_next(&t, &ev)) {
		n_rel++;
		n_del += ev.minted;
	}
	CHK(n_rel == 2 && n_del == 1);
	/* Two parents can carry the SAME VID and they are different segments: `p0.11` and
	 * `p1.11` are two netdevs, and a table keyed by VID alone would have merged them. */

	/* ---- the bounds are REPORTED, never silently untracked. */
	reac_topo_init(&t);
	for (int i = 0; i < REAC_TOPO_MAX_PARENTS; i++) {
		char pn[IFNAMSIZ];
		snprintf(pn, sizeof pn, "eth%d", i);
		CHK(reac_topo_watch(&t, pn) == 0);
	}
	CHK(reac_topo_watch(&t, "one-too-many") == -1);
	CHK(t.unbounded == 1);
	for (int i = 0; i < REAC_TOPO_MAX_VLANS + 4; i++)
		reac_topo_saw(&t, "eth0", REAC_TOPO_TAGGED, (uint16_t)(100 + i), SEC);
	CHK(reac_topo_count(&t, "eth0", REAC_TOPO_VLAN_HEARD) == REAC_TOPO_MAX_VLANS);
	CHK(t.unbounded == 5);

	printf("OK: topology — the accelerated tag and the in-buffer tag both classify, the "
	       "parent's untagged copy is told apart only by auxdata, QinQ takes the outer "
	       "VID, VID 0 mints nothing, a VID is ensured once, minted and adopted stay "
	       "distinct through release, silence past the hold releases and a failed ensure "
	       "waits its window\n");
	return 0;
}
