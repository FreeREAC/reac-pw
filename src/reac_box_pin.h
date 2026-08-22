/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * `--box MODEL[:LABEL]` — the operator's PIN for a fixed installation.
 *
 * reac-pw is a daemon in its own right. A permanent rig wants its patch to exist before
 * the box is powered — nodes named and sized from boot, not appearing halfway through a
 * soundcheck — and the operator, not the wire, decides what that box is called. openmixer
 * is the opposite case and autodetects everything; the two are complementary rather than
 * alternatives (operator ruling, 2026-08-22).
 *
 * WHAT THIS PIN DELIBERATELY DOES NOT DO, and why the flag was retired once already.
 * Before 2026-08-05 `--box` pre-fabricated a complete head-amp enrollment for slots
 * 0x20..0x2f before anything had been seen on the wire. A cold-connect JOIN carries no
 * width, so a box reaching GRANTING before declaring its model was granted an enrollment
 * built from a GUESS — and a box granted slots it does not own "links, streams audio, and
 * silently ignores every head-amp record" (commit 902bf39). Recognition corrected it a
 * moment later for boxes in the fixed matrix, which is why it stayed latent.
 *
 * So the pin restored here declares **node geometry and label, and nothing else**. It does
 * not allocate, enroll or grant: `reac_master_set_box` remains the only door in, fed only
 * by what a box declares about itself. The pin says what to CALL the ports and how many to
 * make; the wire says what is actually out there.
 *
 * Where the two disagree, the WIRE WINS and says so exactly once
 * ({@link reac_box_pin_notice}, which survived the retirement for this purpose).
 */
#ifndef REAC_BOX_PIN_H
#define REAC_BOX_PIN_H

#include "reac_ctrl.h"   /* struct reac_box_model, reac_box_model_table */

/**
 * Parse `MODEL[:LABEL]` into a known box model plus the label to name its nodes with.
 *
 * Non-mutating: `*label_out` points into `spec` (just past the first ':') or, when no
 * usable label was given, at the model's own display name — so `--box s1608` alone still
 * produces something an operator recognises in a patchbay rather than an empty name.
 *
 * The model token is matched WHOLE against the fixed matrix. An unknown or partial token
 * is REFUSED rather than guessed: a pin is the operator asserting a fact, and quietly
 * accepting a typo would size the nodes to something nobody chose, leaving a graph that
 * looks plausible and is wrong.
 *
 * Returns 0 on success, non-zero on a NULL/empty spec or an unknown model.
 */
int reac_box_pin_parse(const char *spec,
                       const struct reac_box_model **model_out,
                       const char **label_out);

#endif /* REAC_BOX_PIN_H */
