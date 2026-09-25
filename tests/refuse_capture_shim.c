// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// LD_PRELOAD fault injection for tests/refused-open-releases-its-engine.sh (audit
// 2026-09-24, H1): PipeWire refuses the `reac:capture` stream, and only that one.
//
// That is the failure the audit traced: listener_open has already opened and started the
// slave engine (and, on a join, built reac-playback) when reac_source_node_ensure fails,
// for instance because PipeWire is not up yet at boot. Every other stream is created
// normally, so the open gets exactly as far as it does on a desk before it refuses.
//
// A preload and not a knob in the daemon: the product carries no test-only door, and the
// shim sits at the one seam where the real failure comes from.
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
	if (name && strcmp(name, "reac:capture") == 0) {
		/* pw_stream_new_simple takes ownership of props, on failure too */
		pw_properties_free(props);
		fprintf(stderr, "refuse-capture-shim: refused pw_stream_new_simple(\"%s\")\n", name);
		errno = EIO;
		return NULL;
	}
	if (!real)
		*(void **)&real = dlsym(RTLD_NEXT, "pw_stream_new_simple");
	return real(loop, name, props, events, data);
}
