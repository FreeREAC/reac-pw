// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// A REFUSED NODE SAYS WHY, IN BOTH DIRECTIONS (audit 2026-09-24, M12).
//
// reac-capture had a state_changed handler that names the stream's own error; reac-playback
// had none, so a refused playback node was silent while its capture sibling said why. The
// handler is now one function in reac_node_graph.c, and this test holds:
//   1. ERROR is always printed, with the node's name and the server's own reason;
//   2. any other transition prints only under debug;
//   3. BOTH nodes' stream_events register it (argv[1] is the src dir).

#include "reac_node_graph.h"

#include <pipewire/stream.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
                           else { printf("ok   " __VA_ARGS__); printf("\n"); } } while (0)

/* What one call printed on stderr. */
static char *said(const char *name, int debug, enum pw_stream_state old,
                  enum pw_stream_state st, const char *error)
{
	static char buf[512];
	FILE *tmp = tmpfile();
	int saved = dup(2);
	fflush(stderr);
	dup2(fileno(tmp), 2);
	reac_node_state_changed(name, debug, old, st, error);
	fflush(stderr);
	dup2(saved, 2);
	close(saved);
	rewind(tmp);
	size_t n = fread(buf, 1, sizeof buf - 1, tmp);
	buf[n] = '\0';
	fclose(tmp);
	return buf;
}

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	char *s = calloc(1, (size_t)n + 1);
	if (s && fread(s, 1, (size_t)n, f) != (size_t)n) {
		free(s);
		s = NULL;
	}
	fclose(f);
	return s;
}

int main(int argc, char **argv)
{
	char *s = said("reac-playback.nbn0", 0, PW_STREAM_STATE_CONNECTING,
	               PW_STREAM_STATE_ERROR, "no more input ports");
	CHECK(strstr(s, "reac-playback.nbn0") && strstr(s, "stream ERROR") &&
	      strstr(s, "no more input ports"),
	      "an ERROR names the node and the server's reason, without debug: %s", s);

	s = said("reac-capture", 0, PW_STREAM_STATE_CONNECTING, PW_STREAM_STATE_ERROR, NULL);
	CHECK(strstr(s, "stream ERROR") && strstr(s, "no reason given"),
	      "an ERROR with no text still says so: %s", s);

	s = said("reac-capture", 0, PW_STREAM_STATE_CONNECTING, PW_STREAM_STATE_PAUSED, NULL);
	CHECK(s[0] == '\0', "a normal transition is quiet without debug: '%s'", s);

	s = said("reac-capture", 1, PW_STREAM_STATE_CONNECTING, PW_STREAM_STATE_PAUSED, NULL);
	CHECK(strstr(s, "reac-capture") && strstr(s, "connecting") && strstr(s, "paused"),
	      "under debug a transition names both states: %s", s);

	const char *dir = argc > 1 ? argv[1] : "src";
	const char *nodes[] = { "reac_source_node.c", "reac_sink_node.c" };
	for (size_t i = 0; i < 2; i++) {
		char path[512];
		snprintf(path, sizeof path, "%s/%s", dir, nodes[i]);
		char *src = slurp(path);
		CHECK(src != NULL, "read %s", path);
		if (!src)
			continue;
		const char *ev = strstr(src, "static const struct pw_stream_events stream_events");
		const char *end = ev ? strstr(ev, "};") : NULL;
		const char *reg = ev ? strstr(ev, ".state_changed") : NULL;
		CHECK(reg && end && reg < end, "%s registers a state_changed handler", nodes[i]);
		free(src);
	}

	printf("%s\n", fails ? "FAILED" : "OK");
	return fails ? 1 : 0;
}
