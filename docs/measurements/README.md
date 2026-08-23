# SENS sweep, S-0808, 2026-08-23

Raw per-step readings behind the finding that the head-amp SENS curve is a flat
~1 dB per step over all 56 values and has no duplicate-gain steps.

## The rig

S-0808 input 1 looped to output 1 with a cable; console channel 1; head-amp wire
channel 0 (`reac.headamp.base = 0`). Phantom and pad commanded OFF on that channel
and both records confirmed ON THE WIRE before anything was measured — an output
stage is connected to a mic input and the console's model has reported `phantom:
false` while the metal was lit.

A 1 kHz sine is played into `reac-playback:playback_AUX0` with `pw-play --target=0`
(no auto-link) and ONE explicit link, printed before every phase. The reading is
taken from `reac-capture:capture_AUX0` — the box's own converter output, ahead of
the console — so no fader, trim or bus gain is in the measured path.

## The files

Three passes at three generator levels, so every step is measured with headroom at
one end and margin over the floor at the other:

| file  | generator | steps  |
|-------|-----------|--------|
| `-lo` | -46 dBFS  |  0..27 |
| `-mid`| -58 dBFS  |  0..27 |
| `-hi2`| -72 dBFS  | 12..55 |

They overlap, and the overlap is the check: the two cleaner passes agree to about
0.05 dB across steps 20..27 once the known 26.00 / 14.00 dB generator offsets are
removed. The `-lo` pass drives the box's input ~26 dB harder and shows it — its THD
column sits around -44 dB where the others are near -55 — so it is kept as
corroboration and the curve is built from `-mid` (steps 0..19) and `-hi2` (20..55).

Columns: `tone` is band power over +/-25 Hz around the measured peak (see
`tools/tone-band.py` for why neither a single bin nor broadband RMS will do);
`noise` is everything else; `thd` the harmonic bands; `secs` the length of the
glitch-free window the reading was actually taken over; `ch2_noise` the floor on the
neighbouring input, which never moved off -91.6 dBFS and is the control that the
tone was in the strip we think it was in.
