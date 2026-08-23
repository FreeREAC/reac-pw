# What the daemon emits about its own health, and where it should land

## Why this exists at all

**The fault this was built for raises no xrun.** The TX depth guard discards 256
frames — 64 ms of audio — in one step precisely so that PipeWire never starves.
So no xrun is raised, openmixer's xrun counter never moves, no link drops, the
box stays established, and the console draws a healthy graph while audio goes
missing. Every symptom is absent by construction.

That is the same defect family as everything else this project has been fixing: a
signal that does not observe what it claims to observe. The fix is not a better
guard. It is that the daemon states its own health in numbers.

## What reac-pw emits

Node properties on the `reac-playback` node — the SAME doorway `reac.link-state`,
`reac.box-model`, `reac.master.state` and `reac.pace.source` already travel on. Not
a socket, not a file, not an HTTP endpoint: a second door would be a second ledger
for the same facts, with neither announcing the other. One store, one writer.

Refreshed when a **10 s window closes**, and every value below is a RATE over that
window, not a running total. A consumer that misses three updates has lost
resolution and nothing else.

| property | unit | what it means |
|---|---|---|
| `reac.health.drift-ppm` | ppm, signed | transmit deficit: (nominal − emitted) / nominal. **POSITIVE = fewer frames reached the wire than the rate asks for**, so the TX ring grows and a discard is coming. Unfixed on this rig it read +527; fixed it reads within ±15. |
| `reac.health.discard-fps` | frames/s | frames the depth guard discarded. **Nonzero means audio is being dropped right now**, in 64 ms blocks, with no other symptom anywhere. |
| `reac.health.discard-ms-per-s` | ms/s | the same loss restated as what an operator hears. |
| `reac.health.tx-errors` | count | cumulative `sendto()` failures. Each is a frame built, counter-stamped and never sent. Difference two readings. |
| `reac.health.late-wakes` | count | cumulative slots where the pacer woke more than a full period late. |
| `reac.health.late-wakes-per-s` | 1/s | the same as a rate — this is the honest measure of how close the pacing runs to its margin. |
| `reac.health.catchup-slots-per-s` | 1/s | overslept slots REPAID on the grid. **The correction working**, which an operator should be able to watch rather than trust. |
| `reac.health.dropped-slots-per-s` | 1/s | overslept slots ABANDONED: the debt exceeded the catch-up budget, so it was declared instead of smeared onto the wire. |
| `reac.health.ring-frames` | frames | TX ring depth at the window's close. |
| `reac.health.ring-ms` | ms | the same depth as graph→wire latency. Fell from 44–94 ms to 3.75–18 ms with the slot-debt fix. |
| `reac.health.rate-match-ppm` | ppm, or `n/a` | the correction currently handed to PipeWire's resampler. **`n/a` is not decoration**: a link with no resampler gives the node no rate-match area, and printing `0` there would claim we are steering something we cannot reach — the same lie as a soft meter reading a hardware state. |

The same window also goes to stderr as a `reac-health:` line, because the journal
is where a fault is read after the fact and a property only ever shows the latest
value.

## Where it should land in openmixer

**This lane does not build the openmixer side** — another lane owns that repo.
What it should do there:

- **Its own SSE, latest-wins, skip when late, never accumulate.** That is
  openmixer's settled telemetry contract and these rows were shaped for it: every
  one is already a scalar over a closed window, so dropping an update costs
  resolution and nothing else. Nothing here should be summed by the consumer.
- **Beside the xrun count in `packages/server/src/telemetry.ts`**, not instead of
  it. The whole point is that the xrun count is structurally incapable of seeing
  this, so the two must be readable together or the operator will keep reading the
  one that says everything is fine.
- **Read them where `reac.link-state` is already read.** The engine already
  consumes reac-pw's node properties for the stagebox badge; this is the same
  seam, the same poll, and no new transport.
- **The alarm is `discard-fps > 0`**, and it should be loud: it is the only signal
  on the desk that audio is being lost. `drift-ppm` is the leading indicator —
  discards follow it deterministically at `drift_ppm × fps / 1e6` frames per
  second — so a rising drift is worth showing before anything is lost.
- **Keep the numbers visible after the fix.** Rate matching reduces drift, it does
  not abolish it. `rate-match-ppm` swinging under load is the correction doing its
  job, and an operator should be able to watch that rather than trust it. A large
  and STEADY correction is a fault report, not a success: it means something
  upstream is losing frames and the matcher is the only reason it is inaudible.
