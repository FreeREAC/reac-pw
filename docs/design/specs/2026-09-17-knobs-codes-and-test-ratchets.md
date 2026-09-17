<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# Env knobs are discovered and published; refusals carry a code; tests match codes, not prose

Status: RULED by the operator 2026-09-17, four rulings in one session, verbatim below. Normative
for every `REACPW_*`/`REAC_*` environment knob this daemon or libreac-transport reads, for every
refusal/failure/status line either emits, and for the test scripts under `tests/` that wait on
one. Companion to `2026-08-26-reac-runtime-config.md` (the layered lookup this spec builds on top
of, unchanged) and `2026-09-16-segments-and-roles-are-autodetected.md` (ROLE stays retired; this
spec does not reopen it).

## 1. RULING — a knob is discovered and PUBLISHED, never merely read

*"We should be able to set them and keep them if needed, and announce them when detected, so that
we can manage them; autodetection does not mean obscurity, it's discovery and publish."*

Autodetection (2026-09-16) removed the daemon's *dependency* on env vars for segment/role
discovery — it did not, and does not, forbid an operator override for anything else. The two
rulings are compatible because they answer different questions: what the daemon decides for
itself (segments, roles) is never read from an env var again; what an operator may legitimately
tune (pacing, timing, debug) stays an override, and now says so out loud.

Every such knob:

- keeps its env name, so nothing that already sets one breaks;
- gains the SAME key in `reac-pw.conf`'s existing layered lookup (`reac_conf_lookup`,
  `docs/RATE-AND-CLOCK-CONFIG.md`'s precedence — env above `reac-pw.env`/`reac.env`) unless it is
  libreac-internal in a way that layering cannot reach, in which case the code says precisely why,
  at the read site;
- is announced at start, one line per knob that is SET, grammar
  `reac-pw: S_KNOB_SET knob KEY=value (env|conf)`, plus one summary line
  `reac-pw: S_KNOB_SUMMARY knobs: N set, M default`. An unset knob prints nothing — silence is the
  default's whole story.

`docs/ENV-KNOBS.md` is generated from the same table the announce pass walks
(`tools/gen-env-knobs-doc.py`, `ninja gen-env-knobs-doc`), so the two cannot drift the way the
hand-kept table already had (the old file's own closing line promised sync it could not check).
`tests/test_reac_knob_table.c` fails when a knob the code reads is missing from the table or the
generated doc disagrees with what is committed.

## 2. RULING — error and status lines carry a stable CODE, prose is free to change

*"We need the error codes and not only messages."*

`include/reac/reac_code.h` (libreac, not reac-pw: `reac_master.c` and `reac_pacer.c` emit refusals
of their own, and a token vocabulary read by two binaries is one vocabulary only if it lives where
both already link) declares a closed `enum reac_code` — `RC_E_*` for a refusal/failure, `RC_S_*`
for a notable status — each with a stable token returned by `reac_code_token()`. Every such line
goes through `reac_code_emit(FILE *out, const char *prog, enum reac_code, const char *fmt, ...)`,
which prints `<prog>: <TOKEN> <prose>\n` — the token is the first field, always. Prose may reword
freely; the token is the contract.

Scope is proportionate, not exhaustive: the refusal and status lines this spec's tests key on
(enrolment refused, sizing failed, root refused, segment held, segment heard/dropped/re-heard,
knob set) are migrated. An ordinary debug `fprintf` stays a debug `fprintf`.
`tests/test_reac_code_conformance.c` lists every token and greps `src/*.c` (and libreac's
`transport/src/*.c`) for a bare `fprintf(stderr, "reac-pw:` / `fprintf(stderr, "reac-pacer:`
opening a refusal-shaped sentence (`REFUSED`, `FATAL`, `failed`, `could not`) that does not go
through `reac_code_emit` — a ratchet whose floor may only fall.

## 3. RULING — a test asserts a CODE (and a field), never a prose sentence

*"Let's be precise, let's organise tests to not depend on greps."*

A `wait_for`/`grep` in `tests/*.sh` for a line this spec gave a code migrates to match the token,
plus a `key=value` field where the test needs to read a value back — never the sentence around it.
`tests/box-0832-enrols.sh`'s `reac\.box\.width` match is fixed to `reac\.box-width` (the real
property; the unescaped `.` had made it match almost anything, which is why it never caught the
sizing regression it was written for).

`tests/asserted-lines-have-a-producer.py` is the ratchet for what is NOT migrated: it maps every
remaining `grep`/`wait_for` pattern in `tests/*.sh` to a producing format string in `src/`
(printf conversions normalized to wildcards, `.` escaped, a ternary-built line's branches
enumerated), and refuses a pattern with no producer at all — a pattern that cannot be traced to
anything the daemon can print is a test asserting a typo. A pattern the matcher cannot confidently
resolve is named in `tests/known-debt-grep-patterns.txt` with its reason; that file is a ratchet
too (`tests/debt-list-shrinks.sh`) — it may lose lines, never gain one silently.

## 4. RULING — the five orphaned meson headers, and the mechanism that refuses a sixth

*(mechanical, not a verbatim ruling)* — see the commit `meson: the five orphaned headers sit above
their own test again, and a mechanism refuses the next one`, same day. Fixed the headers at the
lines the brief named; `tests/meson-headers-sit-above-their-test.py` is the standing mechanism,
wired into meson as `meson_headers_sit_above_their_test`, sabotage-verified by a header swap.

## 5. What this spec does not change

Nothing about segment/role autodetection (§1 of `2026-09-16-segments-and-roles-are-autodetected.md`
stands exactly as ruled) — `REACPW_*` pacing/timing/debug knobs were never part of that ruling's
subject and this spec does not reclassify them as decisions the wire makes. `REAC_ROLE*` stays
retired and NAMED-not-honoured, unchanged.
