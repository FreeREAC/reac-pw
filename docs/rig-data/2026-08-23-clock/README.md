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

**`tx_errors` during established audio is ZERO** — confirmed independently by the
daemon's own counter across both A/B legs (`tx_errors=0` on every heartbeat, 8
minutes of established audio). The EAGAIN percentages in the shutdown summaries
are teardown, not steady state, and the open question is closed.

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

Neither leg trimmed, because a freshly started daemon begins with an empty ring
and needs ~11 minutes at leg A's drift to walk from TARGET to HIGH. The discard
rate is a deterministic consequence of the drift — `discard_fps = drift_ppm ×
fps / 1e6` — and §3 is the measurement of it on a daemon that had been up for
hours. Legs C (rate match alone) and D (both) were not reached before the deploy
window; the harness re-runs them unchanged.
