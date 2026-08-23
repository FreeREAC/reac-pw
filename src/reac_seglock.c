// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_seglock.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <net/if.h>
#include <unistd.h>

int reac_seglock_claim(struct reac_seglock *l, const char *ifname)
{
	if (!l || !ifname)
		return -2;
	l->fd = -1;
	l->name[0] = '\0';

	unsigned idx = if_nametoindex(ifname);
	if (idx == 0)
		return -2;

	/* The netns is part of the name so two namespaces driving their own
	 * interfaces never collide, and so the kernel module — which reads the same
	 * inode from the same namespace — computes the identical string. */
	struct stat ns;
	if (stat("/proc/self/ns/net", &ns) != 0)
		return -2;

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	/* sun_path[0] == '\0' selects the ABSTRACT namespace: no filesystem entry, no
	 * stale lock to clean up, released by the kernel whenever the holder dies. */
	int n = snprintf(addr.sun_path + 1, sizeof addr.sun_path - 1,
	                 "reac-pw/segment/%llu/%u",
	                 (unsigned long long)ns.st_ino, idx);
	if (n <= 0 || (size_t)n >= sizeof addr.sun_path - 1)
		return -2;
	snprintf(l->name, sizeof l->name, "@%s", addr.sun_path + 1);

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -2;

	socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
	if (bind(fd, (struct sockaddr *)&addr, len) != 0) {
		int held = (errno == EADDRINUSE);
		close(fd);
		return held ? -1 : -2;
	}

	l->fd = fd;
	return 0;
}

void reac_seglock_release(struct reac_seglock *l)
{
	if (l && l->fd >= 0) {
		close(l->fd);
		l->fd = -1;
	}
}
