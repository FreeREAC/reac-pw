# Health telemetry

reac-pw states its own transmit health in numbers, because a dropped-frame fault
on the wire raises no PipeWire xrun: the TX depth guard discards frames in one
step precisely so PipeWire never starves, so the graph looks healthy while audio
goes missing.

## What it emits

Node properties on the `reac-playback` node — the same doorway `reac.link-state`,
`reac.box-model`, `reac.master.state` and `reac.pace.source` already travel on.
Refreshed when a 10 s window closes; every value is a RATE over that window, not
a running total, so a consumer that misses updates loses resolution and nothing
else. The same window is also written to stderr as a `reac-health:` line.

| property | unit | meaning |
|---|---|---|
| `reac.health.drift-ppm` | ppm, signed | Transmit deficit: (nominal − emitted) / nominal. Positive means fewer frames reached the wire than the rate asks for, so the TX ring is growing toward a discard. |
| `reac.health.discard-fps` | frames/s | Frames the depth guard discarded. Nonzero means audio is being dropped right now. |
| `reac.health.discard-ms-per-s` | ms/s | The same loss restated as audible time per second. |
| `reac.health.tx-errors` | count | Cumulative `sendto()` failures — a frame built and never sent. |
| `reac.health.late-wakes` | count | Cumulative pacer slots that woke more than a full period late. |
| `reac.health.late-wakes-per-s` | 1/s | The same, as a rate. |
| `reac.health.catchup-slots-per-s` | 1/s | Overslept slots repaid on the deadline grid. |
| `reac.health.dropped-slots-per-s` | 1/s | Overslept slots abandoned because the debt exceeded the catch-up budget. |
| `reac.health.ring-frames` | frames | TX ring depth at the window's close. |
| `reac.health.ring-ms` | ms | The same depth as graph-to-wire latency. |
| `reac.health.rate-match-ppm` | ppm, or `n/a` | The correction currently handed to PipeWire's resampler. `n/a` means the link has no resampler to steer, not zero correction. |
| `reac.health.launch-miss-per-s` | 1/s, or `n/a` | **ETF only.** Frames the etf qdisc would not launch, from the kernel's own drop counter over `RTM_GETQDISC`. This is the ETF backend's transmit fault: a frame whose launch time had already passed when it reached the qdisc. `n/a` on the thread backend (no qdisc) and when the dump could not be made — never 0, which would read as "nothing was dropped". |
| `reac.health.qdisc-drops` | count, or `n/a` | **ETF only.** The same counter, cumulative, for differencing by hand. |
| `reac.health.wake-late-us` | µs, or `n/a` | **ETF only.** An upper bound on the worst single WAKE lateness in the window (the debt in slots × the slot period). Harmless while it stays under the lead. |
| `reac.health.wake-lead-us` | µs, or `n/a` | **ETF only.** The lead this segment is running — the budget the figure above is spent against, carried beside it so a consumer never has to know the default to read it. |

The alarm to watch is `discard-fps > 0` — the only signal that audio is actually
being lost. `drift-ppm` is the leading indicator: discards follow it
deterministically at `drift_ppm × fps / 1e6` frames per second.

## `late-wakes` under ETF is the THREAD'S WAKE, not the wire

On the **thread** backend the pacer's wake IS the egress instant, so a late wake is a late
frame and the stderr line says what it always said:

```
… | late 10.20/s (catchup 10.20/s, dropped 0.00/s, worst debt 7 slots) | … | pacer thread
```

On the **etf** backend the thread sleeps to `launch − lead` (2500 µs by default), stamps an
absolute launch time, and the KERNEL releases the frame at that instant. Everything the
thread does inside the lead is invisible to the wire, so `late-wakes` measures the thread
and says nothing about what left. Measured in one namespace under one load: the thread
arm's wire carried 12.1 µs of interval stddev and 208 intervals over 1.5× nominal, the etf
arm's 1.4 µs and 6 — and both reported 10–13 late/s. The desk's own capture put the ratio
near 300×.

So under ETF the line reports the figure that moves, and the wake lateness beside it with
the budget it is spent against:

```
… | launch-miss 0.10/s (qdisc drops 5) | wake-late 13.50/s (worst 6 slots, under 750 us
  of a 2500 us lead; re-base 0.00/s) | … | pacer etf
```

* **`launch-miss`** is the alarm. It is the kernel's count, not ours.
* **`wake-late`** is a health figure about the host, not about the wire. It matters only
  as it approaches the lead — raise `REACPW_PACER_LEAD_US`, or find what is preempting the
  pacer, if `worst … us` gets close to it.
* **`re-base`** is how often the debt exceeded the catch-up budget and the launch grid was
  re-based. Under ETF that budget is the lead less the qdisc `delta`, so a non-zero figure
  here means a stall the lead could not absorb.

`reac.health.late-wakes-per-s` still carries the wake figure on both backends; the ETF
rows above are what tell an operator whether it matters.
