# Fast path — the numbers, before and after

Implements steps 0–2 of
`openmixer/docs/design/specs/2026-08-23-reac-fastpath-and-kernel.md`. `AF_XDP` is off
(§9 step 5) and the kernel module is parked (§9 step 6). This file is the evidence: every
change below has a measurement in front of it and a measurement behind it, taken with the
same instrument, and a change that could not be measured was reverted rather than kept.

Read this file top to bottom — the order is the order the work was done, and one finding
(§B) changes what the spec says the work is.


## The instruments

Three, all reproducible without the rig except the first.

**I1 — pacer transmit jitter, passive.** `tcpdump` on the segment, nanosecond timestamps,
split by source MAC; inter-frame deltas for our 1492-byte broadcast. Passive: it captures a
running master and changes nothing. `tools/pacer-jitter.py` does the arithmetic. The
distribution is reported, and the **trimmed** stddev (0.1% tails removed) beside the raw one
— a single capture-side gap is not pacer jitter, and one such outlier moves a raw stddev by
more than any change in this document.

**I2 — TX frame-ring depth, passive.** The master's own 10 s heartbeat
(`reac_pacer.c`, "ring depth N frames"), which carries the interval min/max and the guard's
cumulative trim counters. Reading it costs nothing and needs no rebuild.

**I3 — per-frame CPU on the hot paths.** `tools/bench_hotpath.c`, built as `bench_hotpath`.
It `#include`s `src/reac_rx.c` so it calls the SHIPPING `feed_frame` rather than a copy that
would drift; `CLOCK_THREAD_CPUTIME_ID`, warm-up discarded, median of 11 passes. Wall time was
rejected because this host also carries the live rig.

Not taken: end-to-end acoustic latency (spec M1) and the one-hour idle/loaded `late_wakes`
runs (M3). Both need the rig, which another lane holds. §F says what that leaves open.


## A. Baseline — 2026-08-23, live rig, unmodified code

I1, 12 s on `enp128s20f0u6` (M-200 profile master, S-0808 box, 48 kHz), both directions:

| source | frames | pps | mean | p50 | stddev | trimmed sd | p99 | max |
|---|---|---|---|---|---|---|---|---|
| **our pacer** (1492 B) | 47 401 | 3998.5 | 250.10 µs | 249.86 | **7.57 µs** | **3.00 µs** | 262.74 | 1074.08 |
| the S-0808's return (340 B) | 47 419 | 3999.9 | 250.01 µs | 250.10 | 11.39 µs | 10.78 µs | 284.43 | 726.94 |

**The spec's §1b holds, re-measured on today's code: our pacer is tighter than the real box
sharing the wire with it** — 3.00 µs trimmed against the box's 10.78. This is the number every
later change must not damage, and it is why nothing in this document trades latency for jitter.

I3, per frame (ns, median of 11):

| case | ns/frame | what it is |
|---|---|---|
| `rx_down_ring40` | 724.9 | 40-ch downstream broadcast decoded into a 40-ch ring |
| `rx_up8_ring40` | **343.9** | **the rig's real path** — the S-0808's 8-ch return into a 40-ch ring |
| `rx_dup` | 13.1 | the duplicate guard's 1492 B memcmp + memcpy |
| `tx_pop_via_popbuf` | 42.9 | the pacer's ring pop + the popbuf→frame memcpy |

At 4 000 fps, 343.9 ns/frame is **0.14% of one core**. That number is the first surprise and
§C is about what follows from it.


## B. The TX ring is not 19 ms, and it is not sawtoothing for the reason recorded

The spec's §1a heads its table with "TX frame-ring depth sawtooths 38..113 frames = 9.5..28 ms,
mean ~19 ms", taken from the comment in `reac_lat.h`. **I2 on the live rig, today, reads
something else entirely:**

    ring depth 254 frames (63.50 ms) [interval min 229 (57.25 ms) max 513 (128.25 ms)]
      | guard trims=300 dropped=77333 frames

- the working depth is **229..516 frames = 57..129 ms**, not 38..113 = 9.5..28 ms;
- the guard is **firing continuously** — 306 trims and 78 884 frames dropped over one run —
  where its own comment says "normal operation NEVER trims";
- each trim drops the depth from the 512-frame HIGH to the 256-frame TARGET: **256 frames =
  3 072 samples = 64 ms of audio, discarded in one step, roughly every 15 seconds.**

The 38..113 figure was a real measurement of a *producer burst*. What is on the wire now is not
a burst at all — it is **drift**. The ring walks from TARGET up to HIGH at about 17 frames per
second (≈4 300 ppm of producer-over-consumer), the guard cuts it back to TARGET, and it walks
again. The sawtooth is the guard's own cycle.

That matters three ways, and each of them contradicts a sentence the spec inherited:

1. **The latency is three times what was recorded.** 57–129 ms of graph→wire buffering, not
   9.5–28. It is still, by a wide margin, the largest term in the system — the spec's central
   claim survives and gets stronger.
2. **The ring is dropping audio, in blocks large enough to hear.** 64 ms is not a click, it is
   a dropout. This is a correctness finding, not a latency one, and nothing in the daemon says
   it is happening except a counter in a heartbeat nobody reads.
3. **Resizing the guard cannot fix it.** The drift has to go somewhere. A lower HIGH/TARGET
   buys lower latency and *more frequent* trims of proportionally smaller size — the same
   audio lost per second, spread differently. The real fix is to absorb the drift
   continuously, in the resampler, which is a rate-matching change and not a cheap win.

§E records what was done about it and what was deliberately left.


## C. What the three copies are actually worth

The spec calls the copies "the strongest single argument in the document" (§2b). Measured, they
are the strongest argument by *count* and a weak one by *cost*. I3, before and after:

| copy | as shipped | with the copy removed | saved | verdict |
|---|---|---|---|---|
| ring write, 40 rows for an 8-ch box | 343.9 | **121–127 (measured after)** | **−218 ns (−64%)** | **taken** |
| the pacer's popbuf→frame memcpy | 42.9 | 30.6 | −12.3 ns (−29%) | **taken** |
| the dup guard's 1492 B memcpy | 13.1 | — | — | **refused, see below** |

(The middle column is the shipping code after the change landed, re-run by `bench_hotpath`. The
ring row beat its own prediction of 133 ns because deleting the 1920-byte per-frame zero-fill —
which only existed to hand those 32 rows over as silence — came free with it.)

**The ring write is the whole prize and the other two are rounding error.** Writing 32 rows of
guaranteed silence into the ring, every frame, costs more than the decode, the zero-fill and
the float conversion put together. Sizing the ring to the box removes it.

**The dup guard was left alone on the evidence.** A 1492-byte memcpy plus a 1492-byte memcmp
measures **13.1 ns** — both buffers are L1-resident and the copy runs at over 200 GB/s. At
4 000 fps that is 0.005% of a core. The guard is the regression pin for the mirror-twin bug
(#82) and swapping a byte compare for a hash trades a proven exact test for a probabilistic one
to recover a twentieth of a microsecond per second. Refused. **This is a case where doing the
work would have been the mistake, and only the measurement says so.**


## D. What the jitter instrument can and cannot resolve

Before grading anything on I1, the instrument had to be graded. **Nine captures were taken, and
five of them turned out to be the same binary** — reac-pw's segment lock refuses a second master
on the same `(netns, ifindex)`, so a restart that was refused left the *previous* binary emitting
and the capture succeeded anyway. The numbers were real and belonged to the wrong build. That
accident produced the most useful number here:

| trimmed stddev on **identical code**, µs | 0.98 · 1.30 · 1.80 · 1.92 · 2.20 · 2.75 · 3.38 · 3.58 · 8.16 |
|---|---|

**The run-to-run spread of the pacer on unchanged code is 0.98 to 8.16 µs.** Nothing in this
document changes the pacer's timing by anything approaching that, and no claim below rests on a
difference smaller than it. The runner now refuses to report a run whose log shows the lock
refusal, and `tools/pacer-jitter.py` rejects a capture that is out of order or has lost more than
2% of its frames — both of which had already produced a plausible-looking wrong answer once.


## E. Change log

| # | change | before | after | jitter after |
|---|---|---|---|---|
| 1 | ring writes the source's width, not the ring's; per-frame 1920 B zero-fill deleted with it | 343.9 ns | **121–127 ns** | inside the noise band |
| 2 | pacer pops straight into the frame it sends | 42.9 ns | 30.6 ns | inside the noise band |
| 3 | sustained-discard detector rewritten; `tx_errors`/`late_wakes` in the heartbeat; `REACPW_GUARD_FLOOR_FRAMES` | — | — | inside the noise band |

Full A/B of the whole change set against the baseline commit, alternating, 18 s each:
before 0.98 and 8.16; after 3.38 and 1.80. **The after values sit inside the before range. Nothing
regressed and nothing improved, on the wire.** Change 2 is kept because it is less code and one
less 2 KB buffer in the `SCHED_FIFO` loop — not because it is faster; 10.8 ns per frame is 43 µs
per second, and saying otherwise would be dressing up a rounding error.

**Change 3 is the one that matters and it is not an optimisation.** The daemon already carried a
diagnostic for exactly the fault that is happening, and it could not fire: it needed twenty
*consecutive* trimming drains against a 200 ms drain cadence, and this fault trims once every
24 s. 356 trims, 0 warnings, positive control on the same log. It now measures discarded frames
per second over three consecutive 30 s windows, and reports the drift in ppm.


## F. Not attempted, and why

**`PACKET_RX_RING` and the BPF filter (spec step 1's last two items).** Both live in
`libreac/src/reac_capture.c`, which this lane is not permitted to edit. **Left as a request to
the libreac lane**, with the caveat that the case for them is weaker than it reads: RX measures
0.7 ms of ring fill against the TX side's 57–129 ms, and the whole per-frame receive path now
costs 121 ns. `SO_ATTACH_FILTER` is still worth building, but for the trunk spec's topology
detector (which is specified as `ETH_P_ALL` + a filter and cannot be written without it), not for
throughput.

**`SO_RCVBUFFORCE` on the capture socket.** Reachable from reac-pw without touching libreac —
`reac_rx.c` already sets `SO_RCVTIMEO` on `cap.fd` from outside. Not done because **it could not
be measured**: demonstrating a receive-buffer win needs induced burst load on a rig held by
another lane, and adding a 32 MB buffer on the strength of "the repacer needed one" is the kind
of unmeasured change this document exists to avoid. It is cheap and it is a one-liner when the
rig frees up.

**Lowering the guard floor.** The knob is in; the sweep is not. Choosing a number needs M5 on the
rig, and §B.3 is the reason not to guess: a lower floor buys latency and pays for it in *more
frequent* dropouts of proportionally smaller size. The audio lost per second does not change,
because the drift sets it.

**AF_XDP and the kernel module.** Off by instruction (spec §9 steps 5 and 6).


## G. The thing to fix next, and it is not on the spec's list

**Give the sink node rate matching.** The 2 693 ppm the graph runs over the wire is the whole
story of §B: at that rate the ring must either grow without bound or discard audio, and the
guard's job is only to choose which. reac-pw has no TX resampler at all — the source node
publishes `io_rate_match` from the counter-slope estimator and the sink node publishes nothing —
so the drift has nowhere to go but a dropout. Every other number in this document is smaller than
this one by three orders of magnitude.

Two things must be measured on the rig before anyone builds it, and neither needs new code now:

- **Where the 2 693 ppm comes from.** A crystal is fifty. This is structural — a resampler ratio,
  a driver rate, or a quantum accounting error — and the fix depends on which.
- **`tx_errors` during established audio** (spec §12 Q2). The heartbeat carries it now, so it is a
  subtraction of two log lines on a running daemon.

