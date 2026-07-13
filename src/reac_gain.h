// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_gain — the pure, RT-safe output-gain staging kernel for reac:playback.
 *
 * The sink node applies PipeWire's SPA_PROP volume/mute to the graph PCM before
 * it is encoded onto the REAC downstream, so a standard controller (wpctl,
 * pavucontrol, WirePlumber channelVolumes) actually attenuates the box outputs —
 * the raw pw_filter has no audioadapter, so nothing else would.
 *
 * Volume SCALE: the value is a LINEAR amplitude multiplier, exactly as
 * SPA_PROP_channelVolumes is defined ("one (linear) volume per channel, 0.0 is
 * silence, 1.0 is without attenuation") and exactly as PipeWire's own
 * audioconvert applies it. 1.0 = unity (0 dBFS passthrough), 0.5 = -6 dB,
 * 0.0 = silence. Keeping the same linear-multiplier semantics the adapter-backed
 * sinks use means wpctl / WirePlumber / pavucontrol treat a REAC box sink
 * identically to a soundcard sink (any cubic taper they show is a client-side
 * display concern applied uniformly to every node).
 *
 * This TU is deliberately free of PipeWire, atomics, allocation and syscalls so
 * it is a pure function the RT path can call and the unit test can exercise
 * directly (the codebase's "pure logic + thin I/O shell" split). */
#ifndef REAC_GAIN_H
#define REAC_GAIN_H

/* Apply a per-sample linear gain ramp to one mono block IN PLACE.
 *
 * For each of the `n` samples, the running gain `cur` steps toward `target` by
 * at most `step` (linear, de-zipper), then multiplies the sample; the gain after
 * the last sample is returned to be carried into the next block. A `target` of
 * 0.0 is mute; unity (1.0 == 1.0) leaves the block untouched. `step` is the
 * per-sample increment (typically 1/ramp_samples, chosen by the caller from the
 * sample rate); step <= 0 snaps straight to `target` (no ramp).
 *
 * RT-safe: no atomics, no allocation, no syscalls — arithmetic only. Pure: the
 * result depends solely on the arguments, which is what the unit test pins. */
float reac_gain_ramp_block(float *buf, unsigned n, float cur, float target, float step);

#endif /* REAC_GAIN_H */
