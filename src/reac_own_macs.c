// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_own_macs.h"

int reac_own_macs_read(struct reac_own_macs *s, const char *netdir)
{
	(void)s; (void)netdir;
	return 0;
}

int reac_own_macs_has(const struct reac_own_macs *s, const uint8_t mac[6])
{
	(void)s; (void)mac;
	return 0;
}

enum reac_own_echo reac_own_echo_screen(const struct reac_own_macs *s, int *said,
                                        const uint8_t *frame, size_t n)
{
	(void)s; (void)said; (void)frame; (void)n;
	return REAC_OWN_ECHO_NONE;
}
