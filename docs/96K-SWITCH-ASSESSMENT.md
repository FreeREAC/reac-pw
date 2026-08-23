# Switching this rig to 96 kHz — cost, risk, and what must be re-measured

**Nothing here was executed.** The rig ran at 48 kHz throughout; every number in
`docs/rig-data/2026-08-23-clock/` was taken at 48 k and none of it transfers
unchanged. This is an assessment, not a result.

## What 96 k changes, arithmetically

Cadence is `fps = rate / 12` at every rate — 12 samples per REAC frame is
invariant. So:

| | 48 kHz | 96 kHz |
|---|---|---|
| frame rate | 4 000 fps | **8 000 fps** |
| slot period | 250 µs | **125 µs** |
| bytes/s downstream, per segment | 1492 B × 4 000 = 6.0 MB/s | **11.9 MB/s** |
| guard HIGH = 512 frames | 128 ms | **64 ms** |
| catch-up budget = 4 slots | 1.0 ms | **0.5 ms** |

Nothing about the frame changes. Twice as many of them go out.

## Does the slot-debt result carry over? NO — and the direction is against us

**The late-wake budget matters MORE at 96 k, not less, and this is the single
thing that must be re-measured before anyone calls the fix verified there.**

The pacer oversleeps by a roughly fixed amount of TIME — it is a scheduler tail,
not a fraction of the period. Measured at 48 k: 356 holes over 375 µs in 90 s,
with the tail running 400–900 µs and 8 excursions past 1 ms. Those same absolute
hiccups, measured against a 125 µs slot instead of a 250 µs one, become debts of
**twice as many slots**:

- a 700 µs oversleep is a 3-slot debt at 48 k and a **6-slot debt at 96 k**;
- the soak's worst observed single debt at 48 k was 3–5 slots, i.e. 375–625 µs of
  wall time. At 96 k that same wall time is **6–10 slots**;
- **the 4-slot catch-up budget is therefore very likely too small at 96 k.** It
  was chosen from a 48 k distribution and it is a slot count, not a duration.

Two consequences, and they pull in opposite directions:

1. More debts exceed the budget, get abandoned rather than repaid, and the drift
   comes back — partially, not fully, but enough that the discard question
   reopens.
2. Raising the budget to keep the same wall-clock coverage (8 slots at 96 k = the
   same 1.0 ms) makes the catch-up BURST twice as long in frames — 8 × 1492 B,
   still only ~95 µs of wire at 1 Gb/s, so the burst is not the constraint. The
   budget should probably be expressed in TIME and converted to slots, not
   configured in slots. **That is a code change, and it is the right one, but it
   must not be made on this reasoning alone — it must be measured at 96 k.**

Also unmeasured at 96 k: whether the pacer thread can hold an 8 kHz cadence at
all under show load. At 48 k it uses 2.8% of a core and misses ~4 deadlines a
second; doubling the wake rate is the case where a `SCHED_FIFO` thread starts
competing with the USB interrupt load, and that is exactly what M3/M4 exist to
answer.

## Does the rate-match result carry over? Partly

The loop's setpoint is `2 × graph_quantum_frames`, which is derived live, so it
re-scales on its own — no constant to change. The bound
(`REAC_SINK_RATE_MATCH_MAX_PPM`, ±5000) is in ppm and is rate-independent. **The
SIGN result carries over unconditionally**: it is a property of the feedback
topology, not of the rate.

What does not carry over is the headroom. At 48 k the loop lived between −1548
and +3571 ppm with the pacer keeping up. If the catch-up budget under-covers at
96 k, the loop is asked to absorb a larger residual drift from a bound that has
not grown, and the margin before saturation shrinks. **A saturated loop is a
silent return of the discards**, which is why `reac.health.rate-match-ppm` is
published and why a steady large correction is a fault report.

## Does the hardware support it?

| | 96 kHz? | evidence |
|---|---|---|
| RME Babyface Pro (graph clock master) | **yes** | `/proc/asound/card0/stream0`: `Rates: 44100, 48000, 88200, 96000, 176400, 192000`. The graph currently runs 192 000 and the reac nodes are 48 k adapters, so a 4:1 resample; at 96 k it becomes 2:1. |
| reac-pw | **yes** | `--rate` accepts 44100/48000/96000 and refuses everything else. 96 000 is now the master default. |
| S-0808 | **unverified on this rig** | a box has no rate setting of its own and locks to the master's cadence, so the question is whether THIS box follows an 8 kHz pace. Never tested here. |
| S-1608 | **unverified on this rig** | same. Note the memory record that the WIRE runs 96 k in the re-pacing work — that is not the same claim as this box establishing at 96 k under our master. |

**"A box has no rate setting" is a design fact, not a measurement that these two
boxes follow 8 000 fps.** Treat both as unproven until one establishes at 96 k and
stays established through a long run.

## The failure mode if the two segments disagree

This is the part that matters most operationally, and it is **not symmetric with a
plain rate mismatch**, because the two segments are independent REAC domains that
meet inside one PipeWire graph.

- **On the wire, nothing breaks.** Each segment has exactly one master and each box
  locks to its own master's cadence. A 96 k segment and a 48 k segment do not see
  each other; there is no shared clock domain to fight over. The seglock already
  makes two masters on ONE segment impossible, which is the failure that actually
  destroys a rig.
- **In the graph, they become two nodes at different rates**, both followers of the
  same 192 kHz driver, each with its own resampler ratio (2:1 and 4:1). PipeWire
  handles that natively — it is the normal case for a graph with mixed-rate
  clients — so the console keeps working and audio keeps flowing.
- **What actually goes wrong is latency and headroom, asymmetrically.** The 96 k
  segment's guard band is 64 ms where the 48 k segment's is 128 ms, its catch-up
  budget covers half the wall time, and its pacer wakes twice as often. So under
  load **the 96 k segment starts discarding while the 48 k one is still fine**, and
  the operator hears one stagebox drop out and not the other. That is a
  particularly nasty symptom to diagnose from the desk, and it is exactly the
  thing `reac.health.discard-fps` now makes visible per node.
- **The one hard failure** is a master brought up at 96 k against a box already
  established at 48 k by a previous master, or vice versa: the box sees a cadence
  it is not locked to and drops to PROBING. It re-establishes at the new rate, so
  it is a few-second dropout rather than a broken rig — but it IS a dropout, and it
  happens on every rate change.

## Recommendation

**Do not switch the live rig now**, and not because 96 k is wrong — the operator's
ruling stands and the default is now 96 000. Because:

1. every measurement behind the fix that is about to be installed was taken at
   48 k, and the one parameter most likely to be wrong at 96 k (the catch-up
   budget) is a slot count derived from a 48 k distribution;
2. neither box has been observed establishing at 96 k on this rig;
3. a rate change is an audible re-handshake on every segment it touches, and the
   operator is validating the console.

**Sequence it as: install the fix at 48 k → soak → then a 96 k trial on ONE
segment, with the catch-up budget re-swept (`REACPW_CATCHUP_MAX_SLOTS`, no
rebuild needed) and `reac.health.slot-debt-max` read to size it.** That knob and
that counter exist precisely so this can be answered with a number instead of an
argument.

---

## The budget, derived from 48 kHz data — no 96 kHz master required

The oversleep is a DURATION. It does not care about the slot period, so the
existing distribution converts directly. From the 30-minute soak's per-window
worst single debt (198 windows of `reac.health.slot-debt-max`, catch-up on):

| worst single debt | wall time | windows | cumulative | the same debt at 96 kHz |
|---|---|---|---|---|
| 0 slots | 0 µs | 17 | 8.6% | 0 |
| 1 | 250 µs | 116 | 67.2% | 2 |
| 2 | 500 µs | 52 | 93.4% | 4 |
| 3 | 750 µs | 7 | 97.0% | 6 |
| 5 | 1250 µs | 2 | 98.0% | 10 |
| 6 | 1500 µs | 1 | 98.5% | 12 |
| 7 | 1750 µs | 1 | 99.0% | 14 |
| 8 | 2000 µs | 2 | 100% | **16** |

**p50 250 µs · p90 500 µs · p95 750 µs · worst 2000 µs.**

**So the defensible 96 kHz budget is 8 slots**, which is the same 1000 µs that
covers ~97% at 48 kHz. That is now the shipping default and it is computed, not
configured: `REAC_CATCHUP_MAX_DEFAULT_US` = 1000, converted at open. At 48 kHz it
still evaluates to exactly the 4 slots that were soaked, so nothing about the
measured configuration changed.

Two honesty notes on this table:

- **These are per-window MAXIMA, not the distribution of individual late wakes.**
  Each row is the worst debt in a 10 s window, so the table over-weights the tail
  — which is the conservative direction for sizing a budget, but it is not the
  same statement as "90% of late wakes are under 500 µs".
- **The control run (catch-up OFF, under heavier load) has a worse tail** — p95 of
  2500 µs and a worst of 3000 µs, i.e. 20 and 24 slots at 96 kHz. A 96 kHz rig
  under show load may well look more like that than like the soak.

## What the conversion CANNOT tell us, and what a minimal trial would prove

The conversion answers one question — what budget the *existing* oversleep
distribution implies at 96 kHz — and it assumes that distribution is unchanged by
running at 96 kHz. **That assumption is the whole risk, and it is exactly what a
trial has to test**, because at 96 kHz the pacer wakes 8 000 times a second
instead of 4 000 and the host's behaviour under that is not a conversion of
anything. Specifically, unmeasurable from 48 kHz data:

1. **Does the oversleep distribution itself get worse?** Twice the wake rate is
   twice the opportunity to be preempted and twice the syscall load in the
   `SCHED_FIFO` loop (the RX drain is up to 8 `recv()` per slot). If the tail
   grows as well as being measured against a shorter slot, the 8-slot budget is
   optimistic and both effects compound.
2. **Can the pacer thread hold 8 kHz at all under show load?** At 48 kHz it uses
   2.8% of a core and misses ~4 deadlines a second. Doubling the wake rate is the
   regime where a `SCHED_FIFO` thread starts competing with USB interrupt load,
   and no 48 kHz number predicts where that turns over.
3. **Do these two boxes establish and STAY established at 8 000 fps?** Never
   observed on this rig. "A box has no rate setting of its own" is a design fact,
   not a measurement of an S-0808 and an S-1608 following our 96 kHz cadence.
4. **What the 2:1 resample does** to the sink adapter's behaviour, versus today's
   4:1 from a 192 kHz graph.

### The minimal trial

**One segment, one box, the S-0808 on `enp128s20f0u6`** — the segment whose
capture is 8 channels rather than the S-1608's 16, and the one every measurement
in `docs/rig-data/2026-08-23-clock/` was taken on, so there is a matched 48 kHz
baseline for every number. Not both segments: a mixed-rate rig is a strictly
harder case and it is not what needs answering first.

- **Cost to start:** one master restart, so a few-second re-handshake on that box.
  If the box does not follow 8 000 fps the segment is down until it is restarted
  at 48 kHz — a rollback of one command, but audibly down in between.
- **Duration: 30 minutes**, to be comparable with the soak that is the baseline.
- **Read:** `reac.health.slot-debt-max` (the whole point — the 96 kHz debt
  distribution in its own units), `discard-fps`, `drift-ppm`, `late-wakes-per-s`,
  `ring-frames`, and the FSM transcript for any `ESTABLISHED -> PROBING`.
- **Pass:** the box establishes and holds it for the full 30 minutes with zero
  drops; `discard-fps` stays 0; the worst debt stays at or under 8 slots for ~97%
  of windows, mirroring the 48 kHz result.
- **Fail, and what it means:** debts routinely above 8 slots says the budget must
  rise (and the burst with it — 16 slots is 190 µs of wire, still tolerable); any
  `ESTABLISHED -> PROBING` says the box does not follow our 96 kHz cadence, which
  is a protocol finding and outranks every tuning question here.
- **It needs no new code.** `REACPW_CATCHUP_MAX_SLOTS` sweeps the budget without a
  rebuild and `slot-debt-max` is already published.

**Do not run it while the console is being validated.** The failure mode is a
segment down, and the operator is at the desk.
