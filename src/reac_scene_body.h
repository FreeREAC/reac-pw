// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* The scene body reac-pw pushes.
 *
 * DATA, NOT PROTOCOL, which is why it stays in the daemon while the transfer that
 * carries it lives in libreac. The framing is the same for everyone; the contents
 * are one desk's state.
 *
 * CAPTURE-DERIVED: recovered from a real M-200i with tools/recover-scene.py (see
 * data/PROVENANCE.md). Our own MAC is substituted at REAC_SCENE_MAC_OFF before it
 * goes on the wire; nothing else is changed. It is a working example, not the
 * right end state — the box reads far more of this body than the three tags it
 * validates, so it also encodes an M-200i's idea of what the desk is. A console
 * that builds its own scene is the correct behaviour, and replacing this is
 * exactly that work. */
#ifndef REAC_SCENE_BODY_H
#define REAC_SCENE_BODY_H

#include <reac/reac_ctrlblk.h>   /* REAC_SCENE_BYTES */

extern const uint8_t reac_scene_placeholder[REAC_SCENE_BYTES];

#endif /* REAC_SCENE_BODY_H */
