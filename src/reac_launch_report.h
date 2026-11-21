// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_launch_report -- what the daemon SAYS about the hardware launch (reac-pw#98): the
 * health line's launch words, the launch-stamp knob's parse, and the refusal of arming the
 * stamps by name. Pure text and numbers, no libreac and no socket, so a unit test reads
 * every word (tests/test_reac_launch_report.c). The callers: reac_sink_node.c's health
 * publish (every window) and its setup (once, off the RT path). */
#ifndef REAC_LAUNCH_REPORT_H
#define REAC_LAUNCH_REPORT_H

#include <stddef.h>

/* REACPW_LAUNCH_STAMPS: a frame count in [1, 1000000], the whole string. 1 and *every set on
 * success, 0 (and *every untouched) on anything else. */
int reac_launch_stamps_parse(const char *v, unsigned *every);

/* The refusal of reac_pacer_launch_stamps_arm by name: "invalid", "busy", "unsupported",
 * "already armed", "out of memory" for -EINVAL, -EBUSY, -ENOTSUP, -EALREADY, -ENOMEM; NULL
 * for 0 (armed) and for any other error, which libreac has already said by itself. */
const char *reac_launch_stamps_refusal(int r);

/* "<backend>, launch <launch>" plus the capitals when the hardware form was installed
 * (`offload_installed`) and the kernel's flags read back as anything but "hardware".
 * Returns what snprintf returns. */
int reac_launch_health_line(char *out, size_t cap, const char *backend, const char *launch,
                            int offload_installed);

/* The launch-stamp instrument as the health line appends it (starts with " | launch-stamps"). */
struct reac_launch_figures {
	unsigned long long paired, unpaired, outside;
	unsigned every;
	double mean_ns, sd_ns;
	long long p50_ns, p99_ns, p999_ns, min_ns, max_ns;
};
int reac_launch_stamps_line(char *out, size_t cap, const char *ifname,
                            const struct reac_launch_figures *f);

#endif
