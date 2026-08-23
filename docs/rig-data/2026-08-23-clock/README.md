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
| **D** | 4 slots | on | −55.7 ppm — **CONTAMINATED, see below; do not quote this as leg D's result** | −15 … +15 ppm at rest | **15–72 frames, 3.75–18 ms** | 0 | 0 |

**Leg D is not a clean measurement of leg D.** A `pnpm build` was running on the
same host through its second half — started by the run's own coordinator, not by
the leg, and not known to this lane until afterwards. So:

- **The clean drift claim is leg B's −8.7 ppm.** Leg D's −55.7 ppm measures a
  loaded host and must not be quoted as what rate matching costs or as a
  regression against B.
- **Leg D's SIGN result is unaffected and stands**, because the sign was read from
  the correction's response to the ring depth, not from the drift figure. Load
  moves the depth around; it cannot make a negative-feedback loop cross zero the
  wrong way.
- **The contamination is itself good evidence, and it is kept for that reason** —
  as an unplanned load test, labelled as one. See §5.

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

**THE TRAP THAT NEARLY SHIPPED, recorded so the next person does not re-make it:
a guard's TARGET and a controller's SETPOINT are different quantities that happen
to share a variable name.** The first version of this loop servoed to the depth
guard's `TARGET` — 256 frames, 64 ms — because that is the depth constant sitting
right there in the pacer. But TARGET is a place to drain TO after a pathological
excursion; it is not a depth the ring should sit at. Servoing to it would have
spent ~50 ms of latency making room for the loop and handed back most of what the
slot-debt fix had just won. The setpoint is what the ring NEEDS (two producer
bursts, 42 frames, 10.5 ms), which is a different question from what the guard
drains to, and nothing but asking that question separately catches it.

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

### The unplanned load test (not leg D's result)

The run also produced the answer to whether one lever makes the other
unnecessary, by accident. **A `pnpm build` was running on the same host through
leg D's second half. It was started by the run's coordinator, not by this lane,
and this lane did not know about it until after the leg finished** — which is why
leg D's drift figure is labelled contaminated above. Treat what follows as a load
test that happened to be run, not as leg D: The late-wake rate climbed 0.6/s → 7.8/s, the pacer's drift rose to
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

---

## 7. The 30-minute soak, and the positive control that makes it mean anything

The 300 s legs could not have failed. At the fixed drift the ring needs ~2 hours
to walk one guard band, so "no trim in 300 s" was a test with no power. Two runs
fix that: one that proves the instrument can SEE a discard, and one long enough
that the old behaviour would have produced many.

### 7a. Positive control — catch-up OFF, 420 s

**A probe that reports absence must first prove it can detect presence.**
`reac.health.discard-fps` had never once been observed non-zero, so until this run
a reading of `0.000` was indistinguishable from a counter that does not work.

    guard trims=6  dropped=1544 frames   = 386 ms of audio, in 8 minutes
    discard 25.698 frames/s (6.425 ms/s) on 5 separate health windows
    wire TX -818.9 ppm   ring 232-513 frames (58-128 ms)   tx_errors 0

**The instrument fires.** Six trims in 8 minutes projects to ~22 in 30, which is
the control the soak below is measured against.

This run also **reconciles the 2693 ppm figure this lane could not reproduce.**
Under load its drift reached **+2225 and +2520 ppm** in individual windows, against
527–900 ppm at rest. The earlier number was not wrong; it was a loaded-host
reading of the same fault. Drift here is load-dependent by a factor of four, which
is worth knowing before anyone quotes a single figure for it.

### 7b. The soak — both levers on, 1800 s

    wire TX -4.8 ppm      RX +0.3 ppm        (the cleanest drift of the session)
    guard trims 0         dropped 0 frames   across 194 health windows
    discards              ZERO
    tx_errors             0
    ring depth            11-47 frames (2.75-11.75 ms); heartbeat min 6, max 53
    late_wakes 1595       catchup 1589 (99.6% repaid)   dropped_slots 39

**Against a control that trims ~22 times in the same window, the soak trimmed
zero times.** That is now a claim with power behind it.

### 7c. The tail, which is what the counter was added for

`reac.health.slot-debt-max` per 10 s window, 194 windows:

| worst single debt (slots) | 0 | 1 | 2 | 3 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| windows | 16 | 113 | 52 | 7 | 2 | 1 | 2 | 8 |

**Six windows out of 194 (3%) saw a debt above the 4-slot budget; the worst was
8.** So the budget covers 97% of what a half-hour throws at it, and the 39
abandoned slots are the other 3%. Raising it to 8 would cover this distribution
entirely at a cost of a 95 µs catch-up burst instead of 48 µs — **a defensible
change, but it is a sweep nobody has run, and 4 is what was measured.**

### 7d. One thing that is NOT clean, stated because it would be easy to omit

**The rate-match loop held a standing POSITIVE correction of +1310 … +3810 ppm
for the whole soak, and never went negative.** That is 26–76% of its ±5000 ppm
authority consumed at rest.

It is not a sign error — leg D settled that by watching the correction cross zero
and reverse. It is a MEASUREMENT-PHASE error in this loop: the RT callback reads
the ring depth BEFORE pushing the quantum's frames, so it reads about one quantum
(21 frames) lower than the depth the ring actually settles at. The loop is
therefore servoing honestly to a setpoint it is comparing against a
phase-shifted measurement, and it holds a standing correction to sit there.

Consequences, plainly:

- **It did not cause a discard in 30 minutes and the drift is −4.8 ppm**, so it is
  not hurting anything today.
- **But it eats most of the loop's headroom**, and the whole reason the applied
  correction is published is that a large steady correction is a fault report
  rather than a success. This one is reporting a fault in itself.
- The fix is to measure the depth after the push (or add the quantum to the
  reading), which should bring the standing correction near zero and restore the
  full ±5000 ppm of margin. **It has not been made and has not been measured, so
  it is not claimed.**
