# Where the drift comes from, and what each lever did — live rig, 2026-08-23

Raw evidence, not a summary. Every number below can be re-taken with
`tools/clock-drift.py` and `tools/ring-depth.sh`; `runab.sh` is the exact harness
that produced the A/B legs, refusals included.

Segment: **enp128s20f0u6**, M-200 profile master, S-0808 box, 48 kHz, 4000 fps.
`enp131s0` (the S-1608, whose 16 capture channels are linked to the console and
may carry the operator's live mic) was **never touched**.

> Noise-floor caveat for anyone comparing LEVEL data to this: the S-0808's
> in1->out1 loop cable was removed by the operator around the time of these runs.
> It does not affect anything here — every measurement in this directory is a
> COUNT or a TIME (packets, frames, sequence counters, ring depth), and none of
> them reads a sample value.

## 1. It is not an oscillator. Three independent clocks, all fine.

| clock | measured | instrument |
|---|---|---|
| RME Babyface Pro, the graph's driver, vs `CLOCK_MONOTONIC` | **−5.6 ppm** | `/proc/asound/card0/pcm0p/sub0` `hw_ptr`/`tstamp`, 30 s |
| S-0808 return on enp128s20f0u6 | **−6.1 ppm** | NIC `rx_packets`, 180 s |
| S-1608 return on enp131s0 | **−20.0 ppm** | NIC `rx_packets`, 180 s |
| **our pacer, enp128s20f0u6** | **−750.5 ppm** | NIC `tx_packets`, 180 s |
| **our pacer, enp131s0** | **−761.6 ppm** | NIC `tx_packets`, 180 s |

The graph runs at **192 kHz** (`clock.rate=192000`, quantum 1024, RME elected
driver) and the reac nodes are 48 kHz adapters, so there is a 4:1 resampler in the
path — and it is not the problem either: the reference behind it reads −5.6 ppm.

Two independent processes, a USB NIC and a PCI NIC, produce the **same** deficit.
That is not jitter and it is not a crystal. The transmit deficit is ours.

The figure this lane was handed (2693 ppm, a discard every 24 s) did not
reproduce. Today reads 750–900 ppm and a discard every 60–95 s. Same fault, same
sign, different load — which is why the instruments are committed with the data.

## 2. It is not `sendto()`. The sequence counter settles it.

`baseline-installed-counter-gaps.txt` — 90 s capture of the pacer's own frames,
snaplen 20, reading the REAC sequence counter at offset 14:

    357 969 frames over 89.573 s   (the probe sees traffic)
    emitted 3996.3936 pps   −901.6 ppm vs 4000
    time holes > 1.5 slots: 356 (3.97/s), 104.8 ms of wire time inside them
    sequence-counter gaps:  0  (0 frames built and never sent)

The pacer advances the counter **once per loop iteration**. A frame built and then
lost to `sendto()` EAGAIN leaves a counter GAP; a slot the loop never ran leaves
the counter CONTIGUOUS across a hole in time. All 357 968 consecutive deltas are
exactly 1.

**READ THIS SENTENCE IF YOU READ NOTHING ELSE HERE: 357 968 consecutive frames
left the NIC with a sequence-counter delta of exactly 1, across 356 holes in
time — so not one frame was built and lost, and every hole is a slot the pacer
never ran. We did not drop packets. We overslept and threw the work away.**

**`tx_errors` during established audio is ZERO.** Confirmed independently by the
daemon's own counter across all three A/B legs — `tx_errors=0` on every single
heartbeat through **thirteen minutes** of established audio. **The 26.5% and
84.6% EAGAIN figures in the shutdown summaries describe TEARDOWN, not running
audio, and must not be quoted as if they described running audio.** Both runs
that produced them ended in PROBING; the open question is closed.

So the deficit is **overslept slots being abandoned**: `deadline = now + period`
re-bases the grid onto the hiccup and the slots are gone for good.

## 3. The baseline sawtooth, from the unmodified installed daemon

`baseline-installed-ring-depth.txt` — TX ring depth read from the
`ProcessLatency` the daemon already publishes, 2 s polls over 430 s:

    depth range      383 .. 5759 samples  =  32 .. 480 frames  =  8 .. 120 ms
    mid-cycle rise   39.1 and 41.9 samples/s  =  813 and 873 ppm
    guard trims      4 in 430 s, i.e. one per ~60–95 s
    each trim        512 -> 256 frames = 256 frames = 64 ms of audio, in one step

256 frames per ~85 s = **0.75 ms of audio lost per second**, delivered as a 64 ms
dropout. No xrun is raised, because preventing one is the guard's purpose.

## 4. The A/B — same binary, same segment, 240 s windows after a 60 s settle

Verified per leg that the process under test is the one built here
(`/proc/<pid>/exe`), that the segment lock was not refused, and that only this
build emits `reac-health:` lines at all.

| leg | catch-up | rate match | wire TX, 240 s | drift, daemon's own 10 s windows | TX ring depth | guard trims | tx_errors |
|---|---|---|---|---|---|---|---|
| **A** | off (`-1`) | off | **−526.7 ppm** | +51 … +1273 ppm | **176–378 frames, 44–94 ms** | 0 | 0 |
| **B** | 4 slots | off | **−8.7 ppm** | −15 … +6 ppm (one +234 outlier) | **22–62 frames, 5.5–15.5 ms** | 0 | 0 |
| **D** | 4 slots | on | **−55.7 ppm** (300 s, under a parallel `pnpm build`) | −15 … +15 ppm at rest | **15–72 frames, 3.75–18 ms** | 0 | 0 |

Leg B, cumulative over the run: `late_wakes=423`, of which **`catchup=417`
repaid on the grid** and only **48 slots abandoned** — against leg A's
`late_wakes=374, catchup=0, dropped_slots=557`.

**Lever 1 alone removes the deficit: −526.7 ppm to −8.7 ppm, a factor of 60.** The
RX side reads +0.7 ppm on the same window, so the wire is now within a part per
million of the box.

And it is not only a correctness fix. The ring no longer has to grow, so the
graph→wire buffering **falls from 44–94 ms to 5.5–15.5 ms** — a 66 ms latency
reduction that no other item in the fast-path spec comes close to, taken by
deleting one line that threw frames away.

## 5. Leg D — the SIGN of the rate-match loop, settled on live hardware

A rate matcher whose feedback sign has never been observed on hardware is an
unverified guard, and an unverified guard is decoration. The sign was therefore
tested **directionally**, not by watching a number improve — a wrong-sign loop can
improve a number transiently before it runs away, so "the drift got better" proves
nothing.

The test: the loop servos the TX ring depth to a setpoint of two producer bursts
(2 x 21 = 42 frames). Leg B leaves the ring at ~22 frames, i.e. **below** the
setpoint. A correctly-signed loop must then apply a **POSITIVE** correction (ask
the resampler for MORE samples) and the depth must rise. An inverted loop applies
a negative correction, the ring empties to zero and the pacer emits FILLER, which
is audible.

Observed, from the daemon's own `reac.health.rate-match-ppm` against its own ring
depth over 300 s:

| ring depth (frames) | correction applied (ppm) |
|---|---|
| 15 | **+3571** |
| 17–20 | +3214 … +3452 |
| 25 | +2500 … +2857 |
| 34 | +2381 |
| 40 | +2024 … +2143 |
| 47 | +238 … +357 |
| 55 | **−238** |
| 72 | **−1548, −1071** |

**The correction is monotonically anti-correlated with the depth error, and it
CROSSES ZERO at ~50 frames and REVERSES above it.** Crossing zero and changing
sign is something only negative feedback does; positive feedback drives the
correction in the same direction as the error and pins it to a rail. It never
approached either rail (range −1548 … +3571 of ±5000), it never trimmed, and it
never emptied. **The sign is correct and the loop converges. This is not
ambiguous.**

The run also produced the answer to whether one lever makes the other
unnecessary, by accident: a `pnpm build` ran on the same host through the second
half. The late-wake rate climbed 0.6/s → 7.8/s, the pacer's drift rose to
+170…+200 ppm, the ring started climbing — **and the rate matcher took up the
slack, swinging to −1548 ppm and holding the depth.** Lever 1 alone would have
left ~200 ppm under that load and walked the ring to a trim. So:

- **Lever 1 removes the cause** (527 → 8.7 ppm) and is the one that must ship.
- **Lever 2 catches the residual under load** and is not redundant.
- Neither on its own is the whole answer, and lever 2 on its own would have been
  the wrong answer: it would have made a structural 527 ppm loss inaudible
  instead of absent.

## 6. What was not reached

Leg C (rate match alone) was dropped to spend the window on the configuration
that actually ships. Legs A and B did not trim, because a freshly started daemon
begins with an empty ring
and needs ~11 minutes at leg A's drift to walk from TARGET to HIGH. The discard
rate is a deterministic consequence of the drift — `discard_fps = drift_ppm ×
fps / 1e6` — and §3 is the measurement of it on a daemon that had been up for
hours. Leg D ran 300 s and did not trim either, which at its measured drift is
what should happen: 55.7 ppm is 0.22 frames/s, i.e. one trim every 19 minutes if
the loop did nothing — and the loop is what stops it reaching there at all.

The rig was RESTORED to `/usr/bin/reac-pw` on enp128s20f0u6 with the original
command line after leg D; both segments re-established, exactly two masters.
