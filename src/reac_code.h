// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_code — ONE stable token vocabulary for every refusal, failure and notable
 * status line reac-pw prints.
 *
 * "We need the error codes and not only messages" (operator, 2026-09-17). Before this
 * there was no code vocabulary at all — matching a refusal meant matching the sentence,
 * and the sentence is free to be reworded (it was, repeatedly, by lanes that made the
 * prose more honest — see git log on main.c's fprintf lines). `reac_code_emit` puts the
 * TOKEN first, always, so the two can move independently: prose for a human, a token for
 * a script or a log scraper.
 *
 * STILL A SEPARATE COPY, NOT YET A THIN ALIAS, though libreac 1.3.0 now ships
 * `include/reac/reac_code.h` with the same shape (docs/design/specs/
 * 2026-09-17-tunables-api-and-shared-refusal-codes.md, the libreac side of this spec's
 * §6 "owed"): reac-pw links the SYSTEM `libreac-devel` package (`pkg-config libreac`,
 * currently 1.2.2), not the sibling checkout, and this lane may not bump the floor in
 * `meson.build` or cut a release (`tools/reac-release` is the main session's) — doing
 * so now would break every build against the still-current system package. Once the
 * floor moves to >= 1.3.0, this file becomes `#include <reac/reac_code.h>` and the two
 * token lists (kept in sync by hand in the meantime — see the libreac copy for the
 * shared subset) merge into one.
 *
 * Scope is proportionate, not exhaustive (2026-09-17 ruling, §2): this covers the
 * refusal/failure lines and the status lines a test keys on. An ordinary debug print
 * stays a debug print — giving every fprintf in this file a code would bury the ones
 * that matter.
 *
 * X-MACRO so the enum, the token table and any enumeration (the conformance test that
 * lists every token, the ENV-KNOBS-shaped doc a future pass could generate) derive from
 * ONE list and cannot drift apart. */
#ifndef REAC_CODE_H
#define REAC_CODE_H

#include <stdarg.h>
#include <stdio.h>

#define REAC_CODE_LIST(X) \
	/* refusals / failures */ \
	X(RC_E_SIZING,          "E_SIZING") \
	X(RC_E_ROOT_REFUSED,    "E_ROOT_REFUSED") \
	X(RC_E_SEGMENT_HELD,    "E_SEGMENT_HELD") \
	X(RC_E_ENROLL_REFUSED,  "E_ENROLL_REFUSED") \
	X(RC_E_LINK_BUDGET,     "E_LINK_BUDGET") \
	/* A listener still held a node pair where it must not have, or an open that failed
	 * had already built one (#108, autodetect spec amendment 2026-09-20 §a). Either way
	 * the pair is destroyed at the code, so the ghost is a searchable event rather than a
	 * node on the graph that nobody can account for. */ \
	X(RC_E_ORPHAN_PAIR,     "E_ORPHAN_PAIR") \
	/* A roster property REMOVAL cannot be delivered: PipeWire applies a NULL-valued dict
	 * item to the CLIENT's copy and the server merges only what is left, so an absent key
	 * is never removed (#106, same amendment §b). Refused, never pretended. */ \
	X(RC_E_ROSTER_REMOVE,   "E_ROSTER_REMOVE") \
	/* There is no roster on the graph: the node could not be created, or could not be
	 * rebuilt after a group left it. Not fatal — the segments are still in the journal —
	 * but a console that reads the roster is reading nothing, and must be able to tell
	 * that from a daemon with no segments. */ \
	X(RC_E_ROSTER_NODE,     "E_ROSTER_NODE") \
	/* status */ \
	X(RC_S_SEGMENT_HEARD,   "S_SEGMENT_HEARD") \
	X(RC_S_BUDGET_YIELDED,  "S_BUDGET_YIELDED") \
	X(RC_S_SEGMENT_UP,      "S_SEGMENT_UP") \
	X(RC_S_SEGMENT_DROPPED, "S_SEGMENT_DROPPED") \
	X(RC_S_KNOB_SET,        "S_KNOB_SET") \
	X(RC_S_KNOB_SUMMARY,    "S_KNOB_SUMMARY") \
	X(RC_E_UNKNOWN_KNOB,    "E_UNKNOWN_KNOB") \
	X(RC_S_NO_OVERRIDES,    "S_NO_OVERRIDES")

enum reac_code {
	RC_NONE = 0,
#define X(name, token) name,
	REAC_CODE_LIST(X)
#undef X
};

/* Never NULL, including RC_NONE ("?" — a caller passing RC_NONE to reac_code_emit is a
 * bug in the caller, not something to hide behind a plausible-looking token). */
static inline const char *reac_code_token(enum reac_code c)
{
	switch (c) {
#define X(name, token) case name: return token;
	REAC_CODE_LIST(X)
#undef X
	case RC_NONE: break;
	}
	return "?";
}

/* Every refusal/failure/notable-status line goes through here, so the code is ALWAYS
 * the first field after the program tag: "<prog>: <TOKEN> <prose>" (the caller's `fmt`
 * supplies the prose and its own trailing '\n'). Prose may reword freely; the token may
 * not move or change — that is the whole point of this file. */
static inline void reac_code_emit(FILE *out, const char *prog, enum reac_code code,
                                   const char *fmt, ...)
{
	fprintf(out, "%s: %s ", prog, reac_code_token(code));
	va_list ap;
	va_start(ap, fmt);
	vfprintf(out, fmt, ap);
	va_end(ap);
}

#endif /* REAC_CODE_H */
