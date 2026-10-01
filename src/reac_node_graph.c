// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// reac_node_graph — see reac_node_graph.h for the three readings and why UNCONNECTED
// is one of them.

#include "reac_node_graph.h"

#include <pipewire/stream.h>
#include <spa/utils/defs.h>
#include <stdio.h>

int reac_node_on_graph(struct pw_stream *stream, const char **why)
{
	const char *reason = "no node was ever created";
	if (stream) {
		const char *err = NULL;
		enum pw_stream_state st = pw_stream_get_state(stream, &err);
		uint32_t id = pw_stream_get_node_id(stream);
		if (st == PW_STREAM_STATE_ERROR)
			reason = err ? err : "the stream is in error";
		else if (st == PW_STREAM_STATE_UNCONNECTED)
			reason = "the stream is not connected — the PipeWire server went away";
		else if (id == SPA_ID_INVALID)
			reason = "the daemon has given it no node id";
		else {
			if (why)
				*why = "on the graph";
			return 1;
		}
	}
	if (why)
		*why = reason;
	return 0;
}

void reac_node_state_changed(const char *nodename, int debug, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error)
{
	if (state == PW_STREAM_STATE_ERROR)
		fprintf(stderr, "reac-pw: %s: stream ERROR — %s (this node is NOT in the graph; "
		        "its patches cannot exist)\n", nodename, error ? error : "no reason given");
	else if (debug)
		fprintf(stderr, "reac-pw: %s: stream %s -> %s\n", nodename,
		        pw_stream_state_as_string(old), pw_stream_state_as_string(state));
}
