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
 * exactly that work.
 *
 * ONE BYTE IS NOT THE M-200i's: `revision` (u2le at +0x14) is 0x0001 here, not the
 * 0x0000 this body was recovered with. reac.ksy: revision is 0 on a V-Mixer desk and
 * 1 on an M-5000, and the box CACHES it and compares before it will re-read the
 * scene's three sub-objects — a body whose revision differs is treated as changed
 * with no further comparison (evidenced in the firmware image and across the corpus).
 * We announce OHRCA in cfea[19]; sending a V-Mixer revision underneath that is a
 * contradiction the box can see. The other two bytes that differ from an M-5000's
 * body (+0x366..367 and +0x22c6..7) are deliberately NOT copied: the ksy records
 * them as M-5000-only UNINITIALISED padding that changes between that desk's own
 * runs, so they carry no meaning to reproduce. */
#ifndef REAC_SCENE_BODY_H
#define REAC_SCENE_BODY_H

#include <reac/reac_ctrlblk.h>   /* REAC_SCENE_BYTES */

extern const uint8_t reac_scene_placeholder[REAC_SCENE_BYTES];

#endif /* REAC_SCENE_BODY_H */
