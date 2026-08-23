// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_ring — single-producer / single-consumer lock-free sample ring.
 *
 * One ring carries the de-interleaved planar audio between the RX feeder
 * thread (producer) and the PipeWire realtime process() callback (consumer).
 * The feeder writes whole REAC frames (40 ch x 12 samples) decoded by
 * libreac's braid core; process() dequeues one PipeWire quantum worth of
 * samples per channel each cycle.
 *
 * Layout is PLANAR float: a contiguous block of `capacity` frames-per-channel
 * for channel 0, then channel 1, ... The producer and consumer share two
 * atomics (head/tail); no locks, no allocation on the hot path. Sized to a
 * power of two so the wrap is a mask.
 *
 * The realtime side NEVER blocks: on underrun process() emits silence and
 * bumps an underrun counter; on producer-side overrun the feeder writes only
 * what fits and drops the NEWEST remainder (SPSC forbids the producer moving
 * tail — the resampler/clock loop, not the ring, absorbs steady drift, and
 * consumer-side trimming of the OLDEST is reac_ring_trim's job).
 */
#ifndef REAC_RING_H
#define REAC_RING_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

struct reac_ring {
	float *buf;              /* planar: channel c at buf + c*capacity */
	uint32_t capacity;       /* per-channel slots, power of two */
	uint32_t mask;           /* capacity - 1 */
	uint32_t channels;       /* rows the CONSUMER reads: 40, the frame's slot count */
	uint32_t wrote_channels; /* rows the producer filled last write (producer-only) */
	_Atomic uint32_t head;   /* producer writes here (next free slot)  */
	_Atomic uint32_t tail;   /* consumer reads here (next valid slot)  */
	_Atomic uint64_t underruns; /* RT-side: quanta that had to be silenced */
	_Atomic uint64_t overruns;  /* feeder-side: frames dropped (ring full)  */
};

/* Allocate a ring holding `capacity_frames` per channel (rounded up to a power
 * of two). Returns 0 / -1. Call OFF the realtime path. */
int reac_ring_init(struct reac_ring *r, uint32_t channels, uint32_t capacity_frames);
void reac_ring_free(struct reac_ring *r);

/* Per-channel frames currently available to the consumer / free for the
 * producer. Cheap, lock-free; safe to call from either side. */
uint32_t reac_ring_readable(const struct reac_ring *r);
uint32_t reac_ring_writable(const struct reac_ring *r);

/* PRODUCER (RX feeder thread). Push `n` per-channel samples from a planar,
 * de-interleaved float source `planar[c*n + s]`, `src_channels` rows wide.
 *
 * SRC_CHANNELS IS THE BOX'S WIDTH, NOT THE RING'S. The frame carries 40 slots and
 * the consumer reads all 40, but an S-0808 fills eight of them; the other 32 are
 * silence by definition. Writing that silence sample-by-sample every frame cost
 * 210 ns of the 344 ns the whole receive path spends per frame -- more than the
 * decode, the zero-fill and the float conversion together (docs/FASTPATH-
 * MEASUREMENTS.md, C). So the producer writes only the rows it has, and the rows
 * past `src_channels` keep whatever they already hold.
 *
 * THAT IS SAFE ONLY BECAUSE THIS FUNCTION KEEPS THEM SILENT. reac_ring_init
 * calloc's the buffer, so an untouched row is zero from birth; and when
 * `src_channels` SHRINKS -- a 16-channel box swapped for an 8-channel one -- the
 * rows it vacates are zeroed here, once, on the change. Without that the old
 * box's audio would circulate forever in rows nothing overwrites: silence that
 * turns back into someone else's microphone. `wrote_channels` is producer-only
 * state and needs no atomic.
 *
 * If the ring can't hold all `n` it writes as many as fit and drops the NEWEST
 * remainder (an overrun): SPSC discipline forbids the producer from moving tail,
 * so it never races the consumer to drop the oldest. Bounded-latency trimming of
 * the oldest is a consumer-side job (reac_ring_trim). Returns frames written. */
uint32_t reac_ring_write(struct reac_ring *r, const float *planar, uint32_t n,
                         uint32_t src_channels);

/* CONSUMER (PipeWire process()). Pop `n` per-channel samples into PipeWire's
 * planar output `dst[c]` buffers (one pointer per channel). On underrun the
 * shortfall is zero-filled and `underruns` is bumped. Returns frames that were
 * real audio (n - shortfall). REALTIME-SAFE: no locks, no syscalls. */
uint32_t reac_ring_read_planar(struct reac_ring *r, float *const *dst, uint32_t channels, uint32_t n);

/* CONSUMER-side latency trim. Drop the OLDEST samples so at most `keep`
 * per-channel remain. SPSC-safe — only the consumer moves `tail`. Call from
 * process() BEFORE reading: when the producer over-fills (the ring would
 * otherwise peg full and the producer's drop-newest would chop the stream every
 * cycle), this bounds buffer latency and keeps the data fresh. Returns frames
 * dropped. REALTIME-SAFE. */
uint32_t reac_ring_trim(struct reac_ring *r, uint32_t keep);

#endif /* REAC_RING_H */
