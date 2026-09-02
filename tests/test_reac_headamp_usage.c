// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Acceptance test: --headamp's usage() text advertises the HEAD-AMP channel
 * space (0..47, REAC_HEADAMP_MAX_CH), not the 40-wide audio fabric (0..39).
 * A S-1608 stagebox at head-amp base 0x20 occupies slots 32..47 — an operator
 * who trusted "0..39" could never reach its upper bank from this flag, even
 * though parse_headamp() itself already accepts the full 0..47 range.
 *
 * Runs the built reac-pw binary with --help (which prints usage() to
 * stderr and exits 2) and reads its own output back, so this is checking what
 * an operator actually sees, not a copy of the string kept in the test. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s PATH-TO-reac-pw\n", argv[0]);
		return 2;
	}

	char cmd[4096];
	/* ASK FOR HELP EXPLICITLY. This used to run the binary with no arguments and
	 * rely on that printing usage — which stopped being true when the packaged
	 * (no-argument) shape became a real start: the daemon hears its segments, so
	 * a bare run listens rather than explains. `--help` is the question this
	 * test is actually asking, and its answer is the same text on every host. */
	snprintf(cmd, sizeof cmd, "%s --help 2>&1", argv[1]);
	FILE *p = popen(cmd, "r");
	if (!p) {
		fprintf(stderr, "FAIL: could not run '%s'\n", cmd);
		return 1;
	}

	char out[16384];
	size_t n = fread(out, 1, sizeof(out) - 1, p);
	out[n] = '\0';
	pclose(p);

	int fails = 0;
	if (!strstr(out, "CH = head-amp channel 0..47")) {
		fprintf(stderr, "FAIL: usage() does not advertise the head-amp bound "
		        "'0..47' (REAC_HEADAMP_MAX_CH-1)\n");
		fails++;
	}
	if (strstr(out, "0..39")) {
		fprintf(stderr, "FAIL: usage() still advertises the audio-fabric bound "
		        "'0..39' for --headamp CH — an S-1608's upper bank (32..47) "
		        "would read as out of range\n");
		fails++;
	}

	if (fails) {
		fprintf(stderr, "---- captured usage() output ----\n%s\n", out);
		return 1;
	}
	printf("OK: --headamp usage() advertises the head-amp channel space (0..47)\n");
	return 0;
}
