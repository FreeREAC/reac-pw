// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_tx — REAC downstream-frame encoder + raw-socket emitter. Builds a
 * 1492-byte FILLER frame: broadcast dst, our OUI src, EtherType 0x8819, u16-LE
 * counter, type 0x0000 (FILLER carries audio and is checksum-exempt), a zero
 * 32-byte control block, 1440 B audio (40 ch x 12 samples x 3 B, the REAC
 * BRAID: obs-h8819 even/odd channel-pair layout == reacdriver's to-device
 * wordswap16 of BE s24 — the same braid as the box's upstream return, #108),
 * 0xC2 0xEA tail.
 *
 * This is the ENCODER only (reac_tx_build) + a standalone direct emitter
 * (reac_tx_emit). It writes a zero control block (FILLER): audio, no link grant
 * yet — the master role's reac_master_stamp() overwrites [18:50] afterwards on
 * EVERY frame it stamps, including FILLER (a real master does not leave the
 * FILLER block zero on the wire either; #130 fix 2). The master JOIN/HOLD
 * handshake that makes a real Roland desk link lives in reac_master (it stamps
 * the cdea/cfea control block, or the FILLER descriptor, over a frame built
 * here); the SCHED_FIFO cadence pacer that clocks the wire lives in reac_pacer.
 * The sink node (reac_sink_node) uses reac_tx_build + the pacer, not reac_tx_emit. */
#ifndef REAC_TX_H
#define REAC_TX_H

#include <stdint.h>
#include <stddef.h>

struct reac_tx {
	int fd;            /* AF_PACKET socket, -1 if not open */
	int ifindex;
	uint8_t src[6];    /* our source MAC (Roland OUI + stand-in) */
	uint16_t counter;  /* free-running u16, +1 per emitted frame */
};

/* Downstream audio byte layouts, selectable at startup via REAC_TX_LAYOUT
 * (the Stage B A/B listen test, docs/VALIDATION-PLAN.md). Default = BRAID,
 * the REAC wire format (reacdriver to-device wordswap16(BE) == obs-h8819
 * convert_to_pcm24lep, listening-validated on a real M-200i == our #108
 * upstream == the zoneA/zoneB M-5000 goldens at coherence 0.99). "plain" is
 * the reac-aes67 reac_decode layout — the (contested) M-5000-generation
 * variant (#135) — kept as the A/B diagnostic: on a de-braiding box it plays
 * a ~-42 dBFS mid-byte hash of the program. */
enum reac_tx_layout {
	REAC_TXL_BRAID = 0,   /* pair braid: even->g[3],g[0],g[1]; odd->g[4],g[5],g[2] */
	REAC_TXL_PLAIN,       /* "plain": (s*40+ch)*3 lo,mid,hi — M-5000-gen claim / diagnostic */
};

/* Parse a REAC_TX_LAYOUT value ("braid"/NULL/"" -> BRAID); -1 if unknown. */
int reac_tx_layout_parse(const char *name);
/* Byte positions (lo,mid,hi) of sample s / channel ch in the 1440 B audio
 * region under `layout`. Bijective over all 1440 bytes for both layouts. */
void reac_tx_layout_pos(int layout, int s, int ch, size_t pos[3]);

/* Build one downstream REAC frame into out[REAC_FRAME_BYTES] from planar float
 * input planar[ch][s] (ns samples/channel, nch channels mapped onto the 40-ch
 * frame; the rest are silent). `counter` is stamped at bytes 14-15 LE; `src` is
 * the 6-byte source MAC. Returns REAC_FRAME_BYTES. No socket needed — pure +
 * unit-testable (round-trips through the braid un-pack, and is asserted
 * byte-identical to reacdriver's wordswap16(BE); see tests/test_reac_tx.c). */
int reac_tx_build(uint8_t *out, float *const *planar, int nch, int ns,
                  uint16_t counter, const uint8_t src[6]);

/* Open an AF_PACKET raw TX socket on `ifname` (needs CAP_NET_RAW). 0 / -1. */
int reac_tx_open(struct reac_tx *tx, const char *ifname);
void reac_tx_close(struct reac_tx *tx);

/* Build + emit one frame (ns samples x nch) on the wire; counter auto-increments.
 * Returns bytes sent, or -1. */
int reac_tx_emit(struct reac_tx *tx, float *const *planar, int nch, int ns);

#endif /* REAC_TX_H */
