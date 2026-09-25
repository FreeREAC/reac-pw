/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * reac_facts_pw.h — THE ONE DOOR TO THE REAC PROTOCOL'S NUMBERS.
 *
 * Every protocol number reac-pw uses is read from reac-protocol's declaration
 * (spec/protocol-facts.yaml -> spec/gen-facts.py -> reac_facts.h), never spelled
 * here. The operator's ruling of 2026-09-25: "declare only once and use it
 * everywhere — a single source of truth; hardcoded constants are the worst
 * practice and a source of nasty bugs". A fact that is not declared there is not
 * declared here either: reac-protocol owns declarations.
 *
 * WHERE THE HEADER COMES FROM. facts/reac_facts.h is a verbatim copy of the
 * generated header, held to reac-protocol by the `facts_drift` test
 * (tools/facts.py drift). The meson option `facts_dir` points the build at another
 * copy; tools/facts-perturb-check.sh uses it to build against a PERTURBED set,
 * where every free number moves, so a literal left in reac-pw disagrees with the
 * header and a test goes red.
 *
 * WHY THIS ORDER. libreac's public headers still define about fifty of the same
 * names by hand, some spelled differently (0xC2 / 0xc2, 0x0600u / 0x0600). They
 * are included first, then reac_facts_undef.h clears every fact name (it is derived
 * from reac_facts.h by name, by the same tool), and reac_facts.h defines them last.
 * So reac-pw compiles against the generated value, with no redefinition diagnosed,
 * and a later #include of those libreac headers is a no-op behind their guards. */
#ifndef REACPW_FACTS_PW_H
#define REACPW_FACTS_PW_H

#include <reac/reac.h>
#include <reac/reac_ctrlblk.h>
#include <reac/reac_identity.h>
#include <reac/reac_ports.h>

#include "reac_facts_undef.h"
#include "reac_facts.h"

/* A FACT INSIDE A STRING. A help text or a log line that states a protocol number
 * spells it through this, so the text is the generated value too:
 *   "rates: " REACPW_STR(REAC_SAMPLE_RATE_48K)  ->  "rates: 48000"
 * (two levels, so the macro's VALUE is stringified and not its name). */
#define REACPW_STR_(x) #x
#define REACPW_STR(x)  REACPW_STR_(x)

/* The legal paces, as text: "44100, 48000 or 96000" and "44100|48000|96000". */
#define REACPW_RATES_OR   REACPW_STR(REAC_SAMPLE_RATE_44K1) ", " \
                          REACPW_STR(REAC_SAMPLE_RATE_48K) " or " \
                          REACPW_STR(REAC_SAMPLE_RATE_96K)
#define REACPW_RATES_BAR  REACPW_STR(REAC_SAMPLE_RATE_44K1) "|" \
                          REACPW_STR(REAC_SAMPLE_RATE_48K) "|" \
                          REACPW_STR(REAC_SAMPLE_RATE_96K)

#endif /* REACPW_FACTS_PW_H */
