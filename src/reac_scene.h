// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_scene — the master's scene-push transfer, as protocol.
 *
 * These constants and the chunker are the CONTROL-PLANE protocol, so they are
 * expressed once and the master FSM only drives them. They sit in their own
 * header rather than in reac_ctrl.h because reac_ctrl.h already includes
 * reac_master.h (for the classifier's verdict enum) and the master needs the
 * body size for its own storage — the implementation is still reac_ctrl.c.
 */
#ifndef REAC_SCENE_H
#define REAC_SCENE_H

#include <stdint.h>
#include <stddef.h>

/* ---- THE SCENE PUSH: the master's enrolment transfer -----------------------
 *
 * After link-up a desk pushes its scene to the box as one bounded transfer:
 *
 *   op-0101 header  — declares the TOTAL (0x22c8) and carries the body's first 24 B
 *   op-0100 chunk   — 26 B of body, x341
 *   op-0102 final   — the last 14 B
 *
 *   24 + 341*26 + 14 = 8904 = 0x22c8
 *
 * The S-1608 accepts the header only when the declared total equals 0x22c8 (the
 * same constant resolved out of its own firmware image), and it completes
 * reassembly ONLY on the final chunk. Completion is what runs its state-4 COMMIT
 * — the sole promoter of staged head-amp into the active table, and the only
 * thing that flushes the phantom groups. A transfer that stops short therefore
 * leaves the box in reassembly for the life of the link: every head-amp record
 * it receives is written to STAGING and never promoted, which is why byte-perfect
 * records have never lit a 48 V LED.
 *
 * Measured on a real M-200i driving an S-1608 (handshake-ctrl-2026-07-11): the
 * 341 chunks go out back-to-back in 0.680 s, the box answers 01 03 0010 once the
 * transfer has completed, and only then does the desk open its grant window.
 *
 * These are PROTOCOL facts, so they live here and are expressed once; the master
 * FSM's only job is to drive the sequence to completion. */
#define REAC_SCENE_BYTES        8904   /* == 0x22c8, the declared total          */
#define REAC_SCENE_HEAD_BYTES     24   /* body bytes carried by the op-0101      */
#define REAC_SCENE_CHUNK_BYTES    26   /* body bytes per op-0100 (its 0x001a)    */
#define REAC_SCENE_TAIL_BYTES     14   /* body bytes carried by the op-0102      */
#define REAC_SCENE_CHUNKS        341   /* op-0100 count for a REAC_SCENE_BYTES body */
/* Steps in one transfer: header + chunks + final. Step 0 is the header, steps
 * 1..REAC_SCENE_CHUNKS the chunks, the last step the final. */
#define REAC_SCENE_STEPS   (1 + REAC_SCENE_CHUNKS + 1)

/* WHAT THE BOX ACTUALLY VALIDATES. The state-4 commit does three four-byte
 * compares before it promotes anything, and fails ALL promotion if any one of
 * them misses — while the transfer still looks complete from outside. Found by
 * executing the S-1608's own task loop over captured control blocks: zeroing the
 * body 128 bytes at a time, exactly 2 of 70 windows break the commit, and they
 * are the ones holding these tags. Everything else in the 8904 bytes can be zero.
 *
 * This is why byte-perfect head-amp records never lit a 48 V LED. "1234" rides
 * the header, but SYSP and SCEN ride op-0100 chunks 32 and 33 — middle chunks,
 * which the old synthetic pattern overwrote. We had never sent two of the three,
 * so the commit refused the body and nothing was ever promoted out of staging.
 *
 * A GENERATED scene must carry all three at these offsets or it will be refused
 * in exactly the same silent way. */
#define REAC_SCENE_TAG_ID_OFF    0x000   /* "1234" — rides the op-0101 header  */
#define REAC_SCENE_TAG_SYSP_OFF  0x368   /* "SYSP" — rides op-0100 chunk 32    */
#define REAC_SCENE_TAG_SCEN_OFF  0x37c   /* "SCEN" — rides op-0100 chunk 33    */

/* The master's own MAC sits INSIDE the body, 6 bytes at this offset — it is the
 * L2 source of the desk that sent the capture the body came from, so a master
 * replaying a recovered body must substitute its own (reac_ctrl_scene_set_mac).
 * On-wire identity must equal the L2 source; a cloned desk MAC is a
 * slave-disconnect trigger, exactly as for the cfea announce. */
#define REAC_SCENE_MAC_OFF     0x340   /* = 832; the desk writes its own here */

/* Build one step of the transfer into a 34-byte [type|block] template (the shape
 * the master stamps into frame [16:50]), checksum applied. `body` is the scene
 * body and `n` its length, which must be REAC_SCENE_BYTES. Returns 0, or -1 on a
 * bad step index or a body that is not a whole transfer. */
int reac_ctrl_build_scene_step(uint8_t blk[34], const uint8_t *body, size_t n,
                               int step);

/* Substitute `mac` into a mutable copy of a scene body at REAC_SCENE_MAC_OFF.
 * Returns 0, or -1 if the body is not REAC_SCENE_BYTES long. */
int reac_ctrl_scene_set_mac(uint8_t *body, size_t n, const uint8_t mac[6]);

/* A scene body of record. PROTOCOL PLACEHOLDER: these are a real M-200i's bytes,
 * recovered from a capture (tools/recover-scene.py) because a body of the right
 * length is what proves the transfer completes. The scene is the MIXER'S STATE,
 * so shipping another desk's body imposes its settings — generating the body
 * from our own console state is the follow-on work. The transfer FRAMING is what
 * this file owns; the values are field contents. */
extern const uint8_t reac_scene_placeholder[REAC_SCENE_BYTES];

#endif /* REAC_SCENE_H */
