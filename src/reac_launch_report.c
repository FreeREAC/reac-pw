// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "reac_launch_report.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int reac_launch_stamps_parse(const char *v, unsigned *every)
{
	char *end = NULL;
	if (!v || !*v)
		return 0;
	unsigned long n = strtoul(v, &end, 10);
	if (end == v || !end || *end || n == 0 || n > 1000000UL)
		return 0;
	*every = (unsigned)n;
	return 1;
}

const char *reac_launch_stamps_refusal(int r)
{
	switch (r) {
	case -EINVAL:   return "invalid";
	case -EBUSY:    return "busy";
	case -ENOTSUP:  return "unsupported";
	case -EALREADY: return "already armed";
	case -ENOMEM:   return "out of memory";
	default:        return r < 0 ? "refused" : NULL;
	}
}

int reac_launch_health_line(char *out, size_t cap, const char *backend, const char *launch,
                            int offload_installed)
{
	return snprintf(out, cap, "%s, launch %s%s", backend, launch,
	                offload_installed && strcmp(launch, "hardware")
	                    ? " (THE HARDWARE FORM WAS INSTALLED AND IS NOT ON)" : "");
}

int reac_launch_stamps_line(char *out, size_t cap, const char *ifname,
                            const struct reac_launch_figures *f)
{
	return snprintf(out, cap,
	                " | launch-stamps [%s] n=%llu (1/%u, unpaired %llu, outside +-10us %llu) "
	                "error mean %+.1f ns sd %.1f ns p50 %+lld p99 %+lld p99.9 %+lld "
	                "min %+lld max %+lld ns",
	                ifname, f->paired, f->every, f->unpaired, f->outside, f->mean_ns, f->sd_ns,
	                f->p50_ns, f->p99_ns, f->p999_ns, f->min_ns, f->max_ns);
}
