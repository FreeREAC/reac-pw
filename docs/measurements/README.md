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

## What came out

Stitched from `-mid` (steps 0..19) and `-hi2` (20..55), referenced to step 0:

| | measured | flat 1 dB/step | libreac's firmware table |
|---|---|---|---|
| span 0x00 -> 0x37 | **54.60 dB** | 55.00 | 48.75 |
| slope, least squares | **0.988 dB/step**, max residual 0.44 dB | 1.000 | non-uniform |

### The three pairs the firmware table predicted would be twins

Each as a rapid A/B/A/B/A alternation so residual drift shows as a mismatch between
the A readings. Run twice — once with the console monitoring the channel and once
with it muted, at different generator levels.

| pair | table predicts | first take | retake | drift control (A spread) |
|---|---|---|---|---|
| 7 -> 8 | 0.00 dB | +0.916 | +1.118 | 0.082 / 0.099 dB |
| 23 -> 24 | 0.00 dB | +1.364 | +1.311 | 0.335 / 0.153 dB |
| 39 -> 40 | 0.00 dB | +0.965 | +0.838 | 0.264 / 0.075 dB |

### The controls, because a null result from an injected signal is worth nothing alone

* **The tone is the cable, not the room.** Cutting the generator link dropped the
  1 kHz component on the measured channel from -31.624 to **-96.61 dBFS** — below
  that channel's own broadband floor of -70.5 dBFS at that gain, so there is no
  acoustic contribution to it at all. Restoring the link came back to -31.621, a
  0.003 dB agreement.
* **The tone is in the strip we think it is.** Inputs 2..7 never moved off
  -91.6 dBFS in any of the 100+ captures, and the level on input 1 followed every
  SENS command.
* **Repeatability.** The same step re-read at one-minute intervals: -31.625,
  -31.879, -31.626, -31.615 dBFS. Three of four inside 0.011 dB; the outlier also
  carried the worst THD of the four, which is how a contaminated capture shows.
* **Clipping.** Checked at the top of the range before sweeping, and every reading
  carries its own peak and THD. The worst THD in the two clean passes is -43 dB; a
  deliberately clipped capture reads -12 dB with the peak at +0.18 dBFS, so the
  detector has been shown to fire.
* **The dB axis is the box's.** The pad, nominally 20 dB, measured 20.12 dB at step
  0x37 and 20.20 dB at step 0x28.
* **The console made no difference.** Seven sweep points re-taken with the channel
  muted at the desk agreed with the room-live sweep: six within 0.04 dB, and the
  seventh (step 12, the reading closest to the floor in that pass) within 0.36 dB.

### What this does NOT settle

The absolute dBu of either endpoint. A loopback measures the SPAN exactly, but the
endpoint needs the box's own converter reference — the dBu it puts out at 0 dBFS
and the dBFS its sensitivity spec refers to — and the loop only ever sees their
sum. For the record, the end-to-end digital loop gain measured **+10.15 dB at step
0x00** and **+64.75 dB at step 0x37**; combine either with a known converter
reference and the endpoint follows. `-10 dBu` at step 0 is inherited from the three
prior readings that already agreed on it, not measured here.
