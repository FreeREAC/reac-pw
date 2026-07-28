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

/* Build one downstream REAC frame into out[REAC_FRAME_BYTES] from planar float
 * input planar[ch][s] (ns samples/channel, nch channels mapped onto the 40-ch
 * frame; the rest are silent). `counter` is stamped at bytes 14-15 LE; `src` is
 * the 6-byte source MAC. Returns REAC_FRAME_BYTES. No socket needed — pure +
 * unit-testable (round-trips through the braid un-pack, and is asserted
 * byte-identical to reacdriver's wordswap16(BE); see tests/test_reac_tx.c). */
int reac_tx_build(uint8_t *out, float *const *planar, int nch, int ns,
                  uint16_t counter, const uint8_t src[6]);

/* Standard Ethernet CRC-32 (IEEE 802.3 / ISO-HDLC, poly 0xEDB88320 reflected —
 * the same algorithm zlib's crc32() and every NIC's hardware FCS unit use) over
 * `len` bytes. Verification vector: reac_eth_crc32("123456789", 9) == 0xCBF43926
 * (the standard CRC-32 check value).
 *
 * This is NOT a REAC field and reac_tx_build/reac_tx_emit never call it — see
 * task #156's RE writeup (docs/MASTER-HARDWARE-VERIFY.md, "downstream OHRCA
 * trailer") and the earlier, already-committed finding for the box-UPSTREAM
 * direction (docs/SLAVE-EMULATION-SCOPE.md W4(a)). The "2-byte OHRCA trailer"
 * some M-5000 downstream captures show (1494 B instead of 1492 B) is exactly
 * the low 16 bits of this CRC-32 over frame[0:REAC_FRAME_BYTES] (byte0 = crc&
 * 0xFF, byte1 = (crc>>8)&0xFF) — i.e. the first 2 of the standard 4-byte
 * Ethernet FCS, which some switch mirror/SPAN taps pass through and others
 * strip. It is computed by NIC hardware, never by REAC application logic, so
 * there is nothing for reac-pw to emit: a real desk's actual wire frame is
 * REAC_FRAME_BYTES (1492) plus whatever FCS its own NIC appends, identical in
 * kind to every other Ethernet frame reac-pw already sends over AF_PACKET.
 * Exposed only as the pure verify function the RE task asked for (see
 * tests/test_reac_tx.c, which reproduces a real captured M-5000 trailer). */
uint32_t reac_eth_crc32(const uint8_t *buf, size_t len);

/* Open an AF_PACKET raw TX socket on `ifname` (needs CAP_NET_RAW). 0 / -1. */
int reac_tx_open(struct reac_tx *tx, const char *ifname);
void reac_tx_close(struct reac_tx *tx);

/* Build + emit one frame (ns samples x nch) on the wire; counter auto-increments.
 * Returns bytes sent, or -1. */
int reac_tx_emit(struct reac_tx *tx, float *const *planar, int nch, int ns);

#endif /* REAC_TX_H */
