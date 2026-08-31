// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_rt — see reac_rt.h for the ladder and the law it implements. */
#include "reac_rt.h"
#include "reac_conf.h"

#include <ctype.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *reac_rt_prio_source_name(enum reac_rt_prio_source s)
{
	switch (s) {
	case REAC_RT_PRIO_SRC_CALLER:  return "set by the caller";
	case REAC_RT_PRIO_SRC_CONFIG:  return REAC_RT_PRIO_KEY;
	case REAC_RT_PRIO_SRC_REFUSED: return REAC_RT_PRIO_KEY " unusable, built-in in force";
	case REAC_RT_PRIO_SRC_BUILTIN: break;
	}
	return "built-in";
}

int reac_rt_prio_parse(const char *value, enum reac_rt_prio_source *src)
{
	enum reac_rt_prio_source dummy;
	if (!src)
		src = &dummy;

	if (!value)
		goto silent;
	while (*value && isspace((unsigned char)*value))
		value++;
	if (!*value)
		goto silent;

	errno = 0;
	char *end = NULL;
	long v = strtol(value, &end, 10);
	if (errno != 0 || end == value)
		goto refuse;
	while (*end && isspace((unsigned char)*end))
		end++;
	if (*end)                      /* trailing junk: "45x", "45 60" */
		goto refuse;
	if (v < 1 || v > 99)           /* the SCHED_FIFO range; 0 is not a FIFO priority */
		goto refuse;

	*src = REAC_RT_PRIO_SRC_CONFIG;
	return (int)v;

silent:
	*src = REAC_RT_PRIO_SRC_BUILTIN;
	return REAC_RT_PRIO_DEFAULT;
refuse:
	*src = REAC_RT_PRIO_SRC_REFUSED;
	return REAC_RT_PRIO_DEFAULT;
}

int reac_rt_prio_preempts_audio(int prio)
{
	return prio >= REAC_RT_PW_CLIENT_PRIO;
}

int reac_rt_prio_resolve(int caller_prio, const char *home,
                         enum reac_rt_prio_source *src)
{
	enum reac_rt_prio_source dummy;
	if (!src)
		src = &dummy;

	if (caller_prio > 0) {
		*src = REAC_RT_PRIO_SRC_CALLER;
		return caller_prio;
	}

	/* NULL iface: the scheduler ladder is a property of the host, so the
	 * per-segment layer is deliberately not consulted for this key. */
	char v[64];
	if (reac_conf_lookup(REAC_RT_PRIO_KEY, NULL, home, v, sizeof v) == REAC_CONF_NONE) {
		*src = REAC_RT_PRIO_SRC_BUILTIN;
		return REAC_RT_PRIO_DEFAULT;
	}
	return reac_rt_prio_parse(v, src);
}

int reac_rt_thread_go(const char *who, int prio, enum reac_rt_prio_source src)
{
	struct sched_param sp = { .sched_priority = prio };
	if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
		fprintf(stderr, "%s: SCHED_FIFO denied (needs CAP_SYS_NICE / rtprio) — "
		                "running SCHED_OTHER, the wire cadence will jitter and a "
		                "box may not link\n", who);
		return -1;
	}

	if (src == REAC_RT_PRIO_SRC_REFUSED)
		fprintf(stderr, "%s: " REAC_RT_PRIO_KEY " is not a whole priority in 1..99 "
		                "— ignored, built-in %d in force\n", who, REAC_RT_PRIO_DEFAULT);

	/* State the POSITION, not a slogan: a line that says "below the graph" while
	 * the thread sits on top of it is worse than no line, because it is the line
	 * an operator will quote back while hunting the xruns somewhere else. */
	fprintf(stderr, "%s: SCHED_FIFO prio %d (%s) — %s the PipeWire graph "
	                "(driver %d, clients %d)\n",
	        who, prio, reac_rt_prio_source_name(src),
	        reac_rt_prio_preempts_audio(prio) ? "AT OR ABOVE" : "below",
	        REAC_RT_PW_DRIVER_PRIO, REAC_RT_PW_CLIENT_PRIO);

	/* A wire clock that outranks the graph steals the cycle that fills its own
	 * ring, and the damage lands as xruns on an interface reac-pw never touches
	 * — nowhere near this daemon in any log the operator will read. Say it here
	 * or it will not be said at all. */
	if (reac_rt_prio_preempts_audio(prio))
		fprintf(stderr, "%s: WARNING — prio %d is at or above the PipeWire graph "
		                "tier (clients %d, driver %d). This thread can preempt the "
		                "audio cycle: expect xruns and clicks on the audio "
		                "interface. Set " REAC_RT_PRIO_KEY " below %d.\n",
		        who, prio, REAC_RT_PW_CLIENT_PRIO, REAC_RT_PW_DRIVER_PRIO,
		        REAC_RT_PW_CLIENT_PRIO);
	return 0;
}
