// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// LD_PRELOAD fault injection for tests/door-without-outputs.sh: PipeWire refuses the
// FIRST `reac:playback` stream, and only that one. Every later one, and every other
// stream, is created normally.
//
// The case it builds: a box WITH outputs whose reac-playback failed to come up (PipeWire
// not back yet, a refused create) while its reac-capture did. For that window the
// segment's door is reac-capture; when the recovery ladder rebuilds reac-playback the
// door must leave reac-capture again, or the console sees two doors.
//
// A preload and not a knob in the daemon, as tests/refuse_capture_shim.c: the product
// carries no test-only door.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <pipewire/pipewire.h>

struct pw_stream *pw_stream_new_simple(struct pw_loop *loop, const char *name,
                                       struct pw_properties *props,
                                       const struct pw_stream_events *events, void *data)
{
	static struct pw_stream *(*real)(struct pw_loop *, const char *, struct pw_properties *,
	                                 const struct pw_stream_events *, void *);
	static int refused;
	if (name && strcmp(name, "reac:playback") == 0 && !refused) {
		refused = 1;
		/* pw_stream_new_simple takes ownership of props, on failure too */
		pw_properties_free(props);
		fprintf(stderr, "refuse-playback-shim: refused pw_stream_new_simple(\"%s\")\n", name);
		errno = EIO;
		return NULL;
	}
	if (!real)
		*(void **)&real = dlsym(RTLD_NEXT, "pw_stream_new_simple");
	return real(loop, name, props, events, data);
}
