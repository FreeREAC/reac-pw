// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_ctrl — REAC control-plane: the cdea/cfea checksum, a classifying parser,
 * and the slave/stagebox-side frame builders (heartbeat, config-announce,
 * cold-connect, upstream return FILLER). The byte layouts are ground-truthed
 * against the real bidirectional captures (reac-captures/, wired-reac-*-bothdirs);
 * the JOIN frames we EMIT (cold-connect, config-announce) are reconstructed from
 * the §13b/§13d transcription + firmware and are flagged experimental until a
 * fresh PHY-link-up rig capture confirms the master's grant-burst.
 *
 * See reac-firmware-re/REAC-CONNECTION-FSM.md for the full spec + evidence grades.
 *
 * ROLE IS SETTLED BY GEOMETRY, NOT BY THE CONTROL PLANE. A classifier here reads
 * what a peer SAYS; the frame length says what it IS. A master's downstream is
 * always the 40-channel solution (1492 B), a stagebox's upstream its own smaller
 * declared width — libreac's reac_frame_is_master_downstream() /
 * reac_frame_channels(). A stagebox strapped to master mode broadcasts and
 * classifies MASTER by every rule below while still emitting a box geometry, and
 * a master never joins another master: that peer is a misconfigured box to
 * report, not a master to follow. Take the length as the arbiter whenever the
 * two disagree.
 *
 * Direction discipline: the master BROADCASTS downstream; we (a virtual stagebox)
 * UNICAST upstream to the learned master MAC with our Roland-OUI src. Control
 * block is the 32 bytes [18:50]; cdea/cfea carry a checksum at [49] such that
 * Sum(frame[18..49]) mod 256 == 0. FILLER (type 00 00) is checksum-exempt. */
#ifndef REAC_CTRL_H
#define REAC_CTRL_H

#include <stdint.h>
#include <stddef.h>

#include "reac_slots.h"    /* the two slot spaces: audio fabric vs head-amp */
#include "reac_master.h"   /* enum reac_master_rx_event (the classifier's verdict) */
#include <reac/reac_ctrlblk.h>  /* THE control block + scene push, in the library */

/* REAC_CTRL_BLOCK_OFF/END/CKSUM_OFF/LEN, the checksum pair, the nested record
 * checksum and the whole scene push live in <reac/reac_ctrlblk.h>: they are the
 * wire format, identical for any REAC implementation, so there is ONE copy and it
 * is in the library. What remains in this header is reac-pw's own control plane —
 * the parser's verdicts, the frame builders and the box-model matrix. */



/* Checksum over the 32-byte control block [18:50]: set frame[49] so the block
 * sums to 0 mod 256. Verify returns 0 when Sum(frame[18..49]) mod 256 == 0. */

/* The underlying sum-to-ZERO rule, on a bare 32-byte block (no frame offsets):
 * set block[31] so Sum(block[0..31]) mod 256 == 0. This is THE cdea/cfea
 * control-block checksum — reac_ctrl_checksum_apply() is this on
 * frame + REAC_CTRL_BLOCK_OFF, and reac_master's 34-byte control templates
 * (type word + block) apply it at template + 2. One implementation; the two
 * offset bases were previously maintained as independent loops. */

/* The INNER record rule, sum-to-0x80: a DT1-style record (TAG.. payload..
 * CKSUM, e.g. the 6-byte head-amp record at frame[34:40]) carries its last
 * byte such that the whole record sums to 0x80 mod 256 (byte-verified on the
 * M-200, m200-headamp-re/DECODE.md). stamp sets rec[n-1]; verify returns 0
 * when Sum(rec[0..n-1]) mod 256 == 0x80. The rule is the record's, not the
 * head-amp's — any future TAG reuses these. */

/* Classify a raw ethernet frame; fills *out. Returns out->kind. master_mac is
 * the ethernet SOURCE for any master frame — callers learn/pin it from
 * out->src when the frame came from the master (broadcast or unicast-to-us). */

/* MASTER-side box-frame classifier (PURE — no socket): decide whether a raw
 * received frame is a box frame the master FSM cares about, and which
 * reac_master_rx_event it is. Returns 0 with out + ev filled, or -1 for
 * anything else (not 0x8819 / not Roland OUI / our own echo / another
 * master's broadcast). Matcher rules (byte-verified zoneA-48k JOIN):
 *   - broadcast type-0000 -> BCAST_FILLER (the presence-flood; diagnostic);
 *   - cdea 04 03, BE len 0x0013/0x0014, then 00 02, checksum valid -> JOIN,
 *     accepted broadcast OR unicast (the box emits it x3 on PHY-up while
 *     still in broadcast mode). Keyed ONLY on block[0:6] + checksum — the
 *     tail (0x41 ...) is device inventory, never matched;
 *   - unicast-to-us box heartbeat (cdea 01 03 0001) sel 0x81 -> UNICAST,
 *     sel 0x00 -> BYE (the explicit disconnect);
 *   - any other unicast-to-us box frame (upstream FILLER 628/340 B,
 *     config-announce sel 0x82, unknown ctrl) -> UNICAST.
 * reac_ctrl_parse's GRANT kind is direction-blind (a box cold-connect and a
 * master grant are the same cdea 04 03 bytes) — this classifier disambiguates
 * by src, not by adding new kinds. */
int reac_ctrl_classify_box_frame(const uint8_t *frame, size_t len,
                                 const uint8_t our_mac[6],
                                 struct reac_ctrl_parsed *out,
                                 enum reac_master_rx_event *ev);

/* The builders, the head-amp record and its DT1 checksum now live in
 * <reac/reac_ctrlblk.h>: they are the wire format, one copy, in the library. */


/* The SENS step -> dBu curve lives in <reac/reac_ctrlblk.h> with the sweep it
 * rests on (reac_headamp_sens_cdb and its inverse). It was spelled out here as
 * well, and a law with two homes is a law that can drift: this copy and the ksy's
 * agreed on 1 dB per step while libreac's table said otherwise, and two of three
 * agreeing is exactly how a number looks confirmed when nobody has measured it. */

#endif /* REAC_CTRL_H */
