// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_upstream — decode the UPSTREAM (stagebox -> master) audio frame.
 *
 * A box's return frame shares the downstream's envelope — 14 B eth + u16 LE
 * counter @14 + 2 B type + 32 B descriptor area (16 x `00 7a` slot words,
 * regardless of box width) = 50 B header, then the audio region, then the
 * 0xC2 0xEA end marker — but is BOX-WIDTH sized:
 *
 *     frame_len = 52 + n_channels * 36        (36 = 12 samples x 3 B)
 *     S-1608 -> 16 ch -> 628 B;  S-0808 -> 8 ch -> 340 B
 *
 * and its audio region is NOT the M-5000 downstream's plain LE sample-major
 * layout: it uses the obs-h8819 even/odd channel-pair byte BRAID. Per time
 * sample, each channel PAIR occupies a 6-byte group; within the group the
 * even channel's s24 LE bytes are (lo,mid,hi) = group[3],group[0],group[1]
 * and the odd channel's are group[4],group[5],group[2].
 *
 * Resolved 2026-07-10 (task #108) against the rig captures in reac-captures:
 * under the braid the loud wired capture's music channel decodes at lag1
 * autocorrelation +0.998 and every idle channel collapses to the mic noise
 * floor; under plain LE every channel reads sign-extension garbage (the
 * historical "45k RMS on all slots" smear). Channel order is plain ascending
 * (input N = wire channel N-1, 0-based) — no further FPGA scramble.
 *
 * Like the downstream frame, the upstream is rate-invariant: always 12
 * samples per frame, the sample rate carried by the packet rate.
 */
#ifndef REAC_UPSTREAM_H
#define REAC_UPSTREAM_H

#include <stdint.h>
#include <stddef.h>

/* frame_len = REAC_UPSTREAM_OVERHEAD + nch * REAC_UPSTREAM_BYTES_PER_CH */
#define REAC_UPSTREAM_OVERHEAD     52  /* 50 B header + 2 B end marker */
#define REAC_UPSTREAM_BYTES_PER_CH 36  /* 12 samples x 3 B */

/* Channel count carried by an upstream frame of `len` bytes, derived from the
 * frame size. Returns -1 unless len = 52 + nch*36 with nch even (the braid
 * packs channel pairs), 2 <= nch < 40. The 40-ch solution (1492 B) is the
 * DOWNSTREAM broadcast, never a box return, and is rejected. */
int reac_upstream_channels(size_t len);

/* Decode a validated upstream frame's audio region into planar 24-bit LE PCM,
 * un-braiding the channel pairs. out must hold nch * 12 samples x 3 B (nch
 * from reac_upstream_channels; REAC_MAX_CHANNELS*12*3 always suffices) and is
 * planar like reac_decode's output: out[(ch*12 + s)*3 + {0,1,2}].
 *
 * raw/len is the full ethernet frame. Validates the 0x8819 ethertype, the
 * frame shape and the end marker. Returns the samples written per channel
 * (12), or -1 on a malformed frame. */
int reac_upstream_decode(const uint8_t *raw, size_t len, uint8_t *out);

#endif /* REAC_UPSTREAM_H */
