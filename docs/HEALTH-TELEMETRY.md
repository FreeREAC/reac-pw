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

The alarm to watch is `discard-fps > 0` — the only signal that audio is actually
being lost. `drift-ppm` is the leading indicator: discards follow it
deterministically at `drift_ppm × fps / 1e6` frames per second.
