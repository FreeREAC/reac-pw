/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * reac_topo — the tag classifier, the per-parent VLAN table, and the ETH_P_ALL tap that
 * feeds them; see reac_topo.h for the measurements this is built on.
 */
#include "reac_topo.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>

#define REAC_ETHERTYPE 0x8819
#define VLAN_CTAG      0x8100   /* 802.1Q */
#define VLAN_STAG      0x88a8   /* 802.1ad — an outer tag on a QinQ trunk */

const char *reac_topo_kind_name(enum reac_topo_kind k)
{
	switch (k) {
	case REAC_TOPO_NOT_REAC: return "not-reac";
	case REAC_TOPO_UNTAGGED: return "untagged";
	case REAC_TOPO_TAGGED:   return "tagged";
	}
	return "?";
}

const char *reac_topo_verb_name(enum reac_topo_verb v)
{
	switch (v) {
	case REAC_TOPO_NONE:    return "none";
	case REAC_TOPO_ENSURE:  return "ensure";
	case REAC_TOPO_RELEASE: return "release";
	}
	return "?";
}

static uint16_t be16at(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

enum reac_topo_kind reac_topo_classify(const void *buf, size_t len, int tci_valid,
                                       uint16_t tci, uint16_t *vid_out)
{
	const uint8_t *b = buf;
	uint16_t vid = 0;

	if (vid_out)
		*vid_out = 0;
	if (!b || len < 14)
		return REAC_TOPO_NOT_REAC;

	/* The kernel's own answer first. VID 0 is a priority tag and names no VLAN, so it
	 * leaves `vid` at 0 and the frame goes on to read as untagged — `<parent>.0` is not
	 * a netdev anyone wants. */
	if (tci_valid)
		vid = (uint16_t)(tci & 0x0fff);

	/* Then whatever tags the driver left in the bytes. The FIRST of them names the VLAN
	 * only if the kernel supplied none; an accelerated outer tag outranks an inner one. */
	size_t off = 12;
	for (int depth = 0; depth < 2; depth++) {
		uint16_t et = be16at(b + off);
		if (et != VLAN_CTAG && et != VLAN_STAG)
			break;
		if (len < off + 4 + 2)
			return REAC_TOPO_NOT_REAC;
		if (vid == 0)
			vid = (uint16_t)(be16at(b + off + 2) & 0x0fff);
		off += 4;
	}
	if (be16at(b + off) != REAC_ETHERTYPE)
		return REAC_TOPO_NOT_REAC;
	if (vid_out)
		*vid_out = vid;
	return vid ? REAC_TOPO_TAGGED : REAC_TOPO_UNTAGGED;
}

/* ---- the table -------------------------------------------------------------------- */

void reac_topo_init(struct reac_topo *t)
{
	memset(t, 0, sizeof *t);
}

static void emit(struct reac_topo *t, enum reac_topo_verb v, const char *parent,
                 uint16_t vid, int minted)
{
	int next = (t->ev_tail + 1) % REAC_TOPO_EVENTS;
	if (next == t->ev_head) {
		t->dropped_ev++;
		return;
	}
	t->ev[t->ev_tail].verb = v;
	snprintf(t->ev[t->ev_tail].parent, IFNAMSIZ, "%s", parent);
	t->ev[t->ev_tail].vid = vid;
	t->ev[t->ev_tail].minted = minted;
	t->ev_tail = next;
}

int reac_topo_next(struct reac_topo *t, struct reac_topo_event *out)
{
	if (t->ev_head == t->ev_tail)
		return 0;
	*out = t->ev[t->ev_head];
	t->ev_head = (t->ev_head + 1) % REAC_TOPO_EVENTS;
	return 1;
}

static struct reac_topo_parent *parent_of(struct reac_topo *t, const char *name)
{
	for (int i = 0; i < REAC_TOPO_MAX_PARENTS; i++)
		if (t->p[i].name[0] && strcmp(t->p[i].name, name) == 0)
			return &t->p[i];
	return NULL;
}

const struct reac_topo_parent *reac_topo_find(const struct reac_topo *t, const char *parent)
{
	for (int i = 0; i < REAC_TOPO_MAX_PARENTS; i++)
		if (t->p[i].name[0] && strcmp(t->p[i].name, parent) == 0)
			return &t->p[i];
	return NULL;
}

const struct reac_topo_vlan *reac_topo_vlan_find(const struct reac_topo *t, const char *parent,
                                                 uint16_t vid)
{
	const struct reac_topo_parent *p = reac_topo_find(t, parent);
	if (!p)
		return NULL;
	for (int i = 0; i < REAC_TOPO_MAX_VLANS; i++)
		if (p->v[i].state != REAC_TOPO_VLAN_FREE && p->v[i].vid == vid)
			return &p->v[i];
	return NULL;
}

int reac_topo_watch(struct reac_topo *t, const char *parent)
{
	if (parent_of(t, parent))
		return 0;
	for (int i = 0; i < REAC_TOPO_MAX_PARENTS; i++) {
		if (t->p[i].name[0])
			continue;
		memset(&t->p[i], 0, sizeof t->p[i]);
		snprintf(t->p[i].name, IFNAMSIZ, "%s", parent);
		return 0;
	}
	t->unbounded++;
	return -1;
}

/* Release every netdev we minted under this parent and forget it. A parent that lost its
 * carrier or its netdev carries nothing on any VLAN, so the sub-interfaces we created for
 * it have no reason to exist; adopted ones are the host's and are only forgotten. */
void reac_topo_unwatch(struct reac_topo *t, const char *parent, uint64_t now_ns)
{
	(void)now_ns;
	struct reac_topo_parent *p = parent_of(t, parent);
	if (!p)
		return;
	for (int i = 0; i < REAC_TOPO_MAX_VLANS; i++)
		if (p->v[i].state == REAC_TOPO_VLAN_SERVED)
			emit(t, REAC_TOPO_RELEASE, p->name, p->v[i].vid, p->v[i].minted);
	memset(p, 0, sizeof *p);
}

void reac_topo_saw(struct reac_topo *t, const char *parent, enum reac_topo_kind kind,
                   uint16_t vid, uint64_t now_ns)
{
	struct reac_topo_parent *p = parent_of(t, parent);
	if (!p || kind == REAC_TOPO_NOT_REAC)
		return;
	if (kind == REAC_TOPO_UNTAGGED) {
		p->untagged++;
		return;
	}
	p->tagged++;
	struct reac_topo_vlan *free_slot = NULL;
	for (int i = 0; i < REAC_TOPO_MAX_VLANS; i++) {
		struct reac_topo_vlan *v = &p->v[i];
		if (v->state == REAC_TOPO_VLAN_FREE) {
			if (!free_slot)
				free_slot = v;
			continue;
		}
		if (v->vid != vid)
			continue;
		v->frames++;
		v->last_ns = now_ns;
		/* A HEARD VID whose ensure failed retries once its window is spent — the
		 * caller sees a second ENSURE and tries again. */
		if (v->state == REAC_TOPO_VLAN_HEARD && v->retry_after_ns &&
		    now_ns >= v->retry_after_ns) {
			v->retry_after_ns = 0;
			emit(t, REAC_TOPO_ENSURE, p->name, vid, 0);
		}
		return;
	}
	if (!free_slot) {
		t->unbounded++;
		return;
	}
	memset(free_slot, 0, sizeof *free_slot);
	free_slot->vid = vid;
	free_slot->state = REAC_TOPO_VLAN_HEARD;
	free_slot->frames = 1;
	free_slot->last_ns = now_ns;
	emit(t, REAC_TOPO_ENSURE, p->name, vid, 0);
}

static struct reac_topo_vlan *vlan_of(struct reac_topo *t, const char *parent, uint16_t vid)
{
	struct reac_topo_parent *p = parent_of(t, parent);
	if (!p)
		return NULL;
	for (int i = 0; i < REAC_TOPO_MAX_VLANS; i++)
		if (p->v[i].state != REAC_TOPO_VLAN_FREE && p->v[i].vid == vid)
			return &p->v[i];
	return NULL;
}

void reac_topo_ensured(struct reac_topo *t, const char *parent, uint16_t vid, int minted)
{
	struct reac_topo_vlan *v = vlan_of(t, parent, vid);
	if (!v)
		return;
	v->state = REAC_TOPO_VLAN_SERVED;
	v->minted = minted ? 1 : 0;
	v->retry_after_ns = 0;
}

void reac_topo_ensure_failed(struct reac_topo *t, const char *parent, uint16_t vid,
                             uint64_t now_ns)
{
	struct reac_topo_vlan *v = vlan_of(t, parent, vid);
	if (!v)
		return;
	v->state = REAC_TOPO_VLAN_HEARD;
	v->minted = 0;
	v->retry_after_ns = now_ns + REAC_TOPO_RETRY_NS;
}

void reac_topo_tick(struct reac_topo *t, uint64_t now_ns)
{
	for (int i = 0; i < REAC_TOPO_MAX_PARENTS; i++) {
		struct reac_topo_parent *p = &t->p[i];
		if (!p->name[0])
			continue;
		for (int j = 0; j < REAC_TOPO_MAX_VLANS; j++) {
			struct reac_topo_vlan *v = &p->v[j];
			if (v->state != REAC_TOPO_VLAN_SERVED)
				continue;
			if (now_ns < v->last_ns + REAC_TOPO_SILENCE_HOLD_NS)
				continue;
			emit(t, REAC_TOPO_RELEASE, p->name, v->vid, v->minted);
			memset(v, 0, sizeof *v);
		}
	}
}

void reac_topo_release_all(struct reac_topo *t)
{
	for (int i = 0; i < REAC_TOPO_MAX_PARENTS; i++) {
		struct reac_topo_parent *p = &t->p[i];
		if (!p->name[0])
			continue;
		for (int j = 0; j < REAC_TOPO_MAX_VLANS; j++) {
			struct reac_topo_vlan *v = &p->v[j];
			if (v->state != REAC_TOPO_VLAN_SERVED)
				continue;
			emit(t, REAC_TOPO_RELEASE, p->name, v->vid, v->minted);
			memset(v, 0, sizeof *v);
		}
	}
}

int reac_topo_is_trunk(const struct reac_topo *t, const char *parent)
{
	const struct reac_topo_parent *p = reac_topo_find(t, parent);
	return p && p->tagged > 0;
}

int reac_topo_untagged_on_trunk(struct reac_topo *t, const char *parent)
{
	struct reac_topo_parent *p = parent_of(t, parent);
	if (!p || p->tagged == 0 || p->untagged == 0 || p->said_untagged)
		return 0;
	p->said_untagged = 1;
	return 1;
}

int reac_topo_count(const struct reac_topo *t, const char *parent, enum reac_topo_vstate st)
{
	const struct reac_topo_parent *p = reac_topo_find(t, parent);
	int n = 0;
	if (!p || st == REAC_TOPO_VLAN_FREE)
		return 0;
	for (int i = 0; i < REAC_TOPO_MAX_VLANS; i++)
		if (p->v[i].state == st)
			n++;
	return n;
}

/* ---- the socket shell ------------------------------------------------------------- */

/* 0x8819, tagged or not, and nothing else. An ETH_P_ALL socket on a trunk without this
 * filter copies every frame on the link to userspace.
 *
 * With an accelerated tag the bytes carry no 802.1Q header at all (measured — see the
 * header), so instruction 1 matches on its own; with an in-buffer tag the ethertype at 12
 * is 0x8100 or 0x88a8 and the real one sits 4 bytes further on, which is what the second
 * and third arms read. Two levels of tag are accepted for the QinQ case. */
static struct sock_filter reac_topo_bpf[] = {
	{ BPF_LD  | BPF_H   | BPF_ABS, 0, 0, 12 },            /* A = ethertype        */
	{ BPF_JMP | BPF_JEQ | BPF_K,   9, 0, REAC_ETHERTYPE },/* plain 0x8819 -> pass */
	{ BPF_JMP | BPF_JEQ | BPF_K,   1, 0, VLAN_CTAG },
	{ BPF_JMP | BPF_JEQ | BPF_K,   0, 6, VLAN_STAG },     /* not a tag -> drop    */
	{ BPF_LD  | BPF_H   | BPF_ABS, 0, 0, 16 },            /* A = inner ethertype  */
	{ BPF_JMP | BPF_JEQ | BPF_K,   5, 0, REAC_ETHERTYPE },
	{ BPF_JMP | BPF_JEQ | BPF_K,   1, 0, VLAN_CTAG },
	{ BPF_JMP | BPF_JEQ | BPF_K,   0, 2, VLAN_STAG },
	{ BPF_LD  | BPF_H   | BPF_ABS, 0, 0, 20 },            /* A = innermost        */
	{ BPF_JMP | BPF_JEQ | BPF_K,   1, 0, REAC_ETHERTYPE },
	{ BPF_RET | BPF_K,             0, 0, 0 },             /* drop                 */
	{ BPF_RET | BPF_K,             0, 0, 0x40000 },       /* pass the whole frame */
};

int reac_topo_tap_open(const char *parent)
{
	int fd = socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK, htons(ETH_P_ALL));
	if (fd < 0)
		return -1;

	struct sock_fprog prog = {
		.len = (unsigned short)(sizeof reac_topo_bpf / sizeof reac_topo_bpf[0]),
		.filter = reac_topo_bpf,
	};
	int saved;
	if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof prog) != 0)
		goto fail;

	/* THE WHOLE POINT OF THIS SOCKET. Without auxdata the tag is invisible and the
	 * detector reports every trunk as an access port. */
	int on = 1;
	if (setsockopt(fd, SOL_PACKET, PACKET_AUXDATA, &on, sizeof on) != 0)
		goto fail;

	unsigned idx = if_nametoindex(parent);
	if (idx == 0)
		goto fail;
	struct sockaddr_ll sll;
	memset(&sll, 0, sizeof sll);
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_ALL);
	sll.sll_ifindex = (int)idx;
	if (bind(fd, (struct sockaddr *)&sll, sizeof sll) != 0)
		goto fail;
	return fd;
fail:
	saved = errno;
	close(fd);
	errno = saved;
	return -1;
}

int reac_topo_tap_next(int fd, enum reac_topo_kind *kind, uint16_t *vid)
{
	uint8_t frame[2048];
	uint8_t control[CMSG_SPACE(sizeof(struct tpacket_auxdata))];
	struct iovec iov = { .iov_base = frame, .iov_len = sizeof frame };
	struct msghdr msg;
	memset(&msg, 0, sizeof msg);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof control;

	ssize_t n = recvmsg(fd, &msg, 0);
	if (n < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;

	int tci_valid = 0;
	uint16_t tci = 0;
	for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
		if (cm->cmsg_level != SOL_PACKET || cm->cmsg_type != PACKET_AUXDATA)
			continue;
		struct tpacket_auxdata aux;
		memcpy(&aux, CMSG_DATA(cm), sizeof aux);
		/* TP_STATUS_VLAN_VALID is what separates "vid 0" from "no tag": the kernel
		 * zeroes tp_vlan_tci for an untagged frame and for a priority-tagged one
		 * alike, and only this bit says which. */
		if (aux.tp_status & TP_STATUS_VLAN_VALID) {
			tci_valid = 1;
			tci = aux.tp_vlan_tci;
		}
	}
	enum reac_topo_kind k = reac_topo_classify(frame, (size_t)n, tci_valid, tci, vid);
	if (kind)
		*kind = k;
	return 1;
}

void reac_topo_tap_close(int fd)
{
	if (fd >= 0)
		close(fd);
}
