# Environment knobs

Every `REACPW_*` / `REAC_*` environment variable read by reac-pw, one line
each. The contract for all of them: **unset = default behavior, byte-identical
on the wire** — a knob whose motivating theory is debunked gets deleted, not
kept around (see the repo discipline; the scene-commit / announce A/B knobs
and `REAC_TX_LAYOUT` were removed under that rule once their hypotheses were
resolved).

| Knob | Effect | Default |
|---|---|---|
| `REACPW_GRANT_DWELL_S` | Master role: hold the recognized-but-ungranted dwell (ENROLL -> grant burst) for N whole seconds. A real M-200 holds a cold box ungranted ~27 s while it climbs its JOIN field; the built-in dwell is ~1.6 s (`REAC_M_GRANT_DWELL_SECONDS_X10`). Only the dwell LENGTH changes — the FSM sequence is untouched. | unset (built-in ~1.6 s) |
| `REACPW_GRANT_DWELL_MS` | Master role: the same dwell in MILLISECONDS, so it can be made SHORTER than the built-in ~1.6 s. Takes precedence over `REACPW_GRANT_DWELL_S`. Measured on an S-4000S: 300 ms establishes in 0.456 s at full declared width, 4/4 trials. Per-firmware — the ~27 s box above is the reason this is not a default. | unset (built-in ~1.6 s) |
| `REACPW_GRANT_ON_DECLARE` | Master role: `0` opts OUT of ending the ENROLL -> grant dwell a short settle after the box has DECLARED and been armed, instead of running the dwell out; `grant_dwell` stays as the CAP for a box that has not declared. Event-driven, so it is also correct for a box that needs a long hold. Measured: S-1608 1.684 s -> 0.134 s, S-4000S 1.756 s -> 0.206 s, both audio-verified through a physical loopback. | **on** (spec §3c) |
| `REAC_DEBUG` | Opt-in diagnostic telemetry on stderr, ~every 2 s: RX feeder counters (ok/dup/other/bad/gaps, locked box MAC) in `reac_rx.c` and source-node ring stats (active channels, peak, fill) in `reac_source_node.c`. Set to any value to enable. | unset (silent) |
| `REACPW_CLOCK_FOLLOW` | Master role: DISCIPLINE the TX cadence to a clock reference instead of free-running on `CLOCK_MONOTONIC` (issue #75). Best available wins: NIC/external PHC > the PipeWire graph clock when driven by locked hardware > the box's counter slope. The slot period is steered continuously by a bounded DLL and the phase is never stepped; the reference in use is printed on every change — with its QUALITY tier since #77, see `REACPW_CLOCK_REF` below — and with none available reac-pw free-runs and says so. Unset = the pacer advances its deadline by the fixed nominal period exactly as before — the discipline is never consulted, so emission and timing are identical. **RIG-GATED — see below.** | unset (free-run) |

| `REACPW_CATCHUP_MAX_SLOTS` | Master role: how many OVERSLEPT slots the pacer repays by staying on its deadline grid instead of re-basing the phase to `now` and losing them. **This is the fix for a MEASURED 527 ppm transmit deficit** (2026-08-23, live rig): the pacer oversleeps ~3.6 slots/s and used to abandon every one, which is the entire reason the TX frame ring grew until the depth guard discarded 256 frames — 64 ms of audio — in one step. Leaving the deadline in the past makes the next `clock_nanosleep` return immediately and the owed slots go out back to back, 12 us of wire each. Bounded because an unbounded catch-up after a real stall would dump hundreds of frames in one burst; past the budget the pacer re-bases exactly as before and COUNTS what it abandoned. `-1` restores the pre-2026-08-23 behaviour. | 4 slots (measured: repays 417 of 423 late wakes, leaves 48 abandoned) |
| `REACPW_RATE_MATCH` | Master role: `1` OPTS IN to publishing `io_rate_match` on the sink, so PipeWire's resampler absorbs the residual graph/wire difference instead of the depth guard discarding it in 64 ms blocks. The sink servos the TX ring depth to two producer bursts, bounded at +-`REAC_SINK_RATE_MATCH_MAX_PPM`. **Default OFF, and not because the loop is wrong:** its sign is verified on hardware (the correction crosses zero and reverses, which positive feedback cannot do) and it caused no discard in a 30-minute soak. It is off because its MEASUREMENT PHASE is wrong — the RT callback samples the ring depth before pushing the quantum, reads ~one quantum low, and holds a standing correction of +1310..+3810 ppm, 26-76% of its authority spent at rest. Fixing that (measure after the push) is queued and needs its own soak and its own before/after, because "near zero at rest" is the whole claim. | unset (**OFF**) |
| `REACPW_CLOCK_REF` | Master role: DESIGNATE which device is the clock reference (issue #77) — a case-insensitive **substring** of the device name, e.g. `Babyface`. The operator knows their hardware; a designated device outranks the name heuristic. It does **not** rescue a structurally unusable reference (an HDMI/DisplayPort sink, a software timer) and it does **not** outrank measured instability — a designated reference that proves jittery is demoted and said so. Only consulted when `REACPW_CLOCK_FOLLOW` is set; on its own it changes nothing. | unset (nothing designated) |
| `REACPW_NO_ENROLL` | Master role: suppress the pre-grant ENROLL (cdea 01 03 000d) for a box whose width is already known. A wire differential across 82 captures found no real desk ever sends this frame to an S-1608 (0/11), while every S-0808 (9/10) and S-4000S (5/5) session gets one. A RIG-TEST SWITCH, not a new default: the rig's S-1608 is currently established WITH this ENROLL in flight, and removing it changes nothing else about the grant burst, dwell timing or the ESTABLISHED transition (`test_reac_master_no_enroll`) — but whether the box still enrols WITHOUT it is untested. Flip deliberately, one box at a time. | unset (ENROLL sent, today's behaviour) |

## `REACPW_CLOCK_REF` — reference quality, and why there is no vendor list

Owning the REAC pace means every stagebox on the segment locks to *our*
rhythm, so the reference behind that rhythm has to be worth propagating.
reac-pw grades the candidate it found on a five-step ladder — `unusable` <
`marginal` < `ungraded` < `good` < `operator-designated` — and prints the tier
next to the device name on every clock line.

The grading rules, in the order they apply:

1. **Structurally disqualified is sticky.** Software timers (`clock.system.*`,
   Dummy-Driver, Freewheel-Driver) and display-derived sinks (HDMI,
   DisplayPort) are `unusable`. Nothing lifts that — not this knob, not a
   stable measurement. A pixel clock chosen to suit a monitor is not a word
   clock, and a quiet ten minutes does not make it one.
2. **The name heuristic can only reject, never promote.** There is deliberately
   no allow-list of "good" vendors: a list like that demotes every
   professional interface nobody has added to it yet. Anything not on the
   disqualified list is `ungraded` — usable, no opinion — and `ungraded`
   outranks `marginal` because `marginal` is a demotion that has to be earned.
3. **This knob outranks the heuristic.** A device whose name contains the
   designated substring is `operator-designated`.
4. **Measurement outranks everything above it.** The DLL's filtered residual
   has a variance, and that variance is a measured stability signal.
   Sufficiently steady over a long enough tracking run promotes to `good`;
   measurably wandering demotes to `marginal`, whatever the badge says and
   whoever designated it. A reference that never gets in band earns no verdict
   at all — that it is stuck at `acquiring` is already the report.

A `marginal` reference is still followed and loudly flagged rather than
ejected: ejecting it mid-run would reset the stability measurement, re-admit
the reference, measure it badly again, and flap a live segment on our own
opinion. Whether `marginal` is good enough to own a segment is the operator's
call (`reac_clock_quality_can_own` says no); free-running while owning a
segment is a legitimate emergency configuration, not a default to slide into.

Campaign/measurement knobs used during rig RE sessions live on their campaign
branches, not on `main` (e.g. the head-amp no-enroll campaign's
`REACPW_NO_ENROLL` / `REACPW_OP0100_BURST` on `wip/headamp-noenroll-campaign`)
— they are documented in the branch's own commits and get folded into code
defaults or deleted when the protocol question closes.

Every knob above is also listed in `reac-pw --help` (the `environment` section
of `usage()` in `src/main.c`). Keep the three places in sync when a knob is
added or removed: the code site, `usage()`, and this file.

## `REACPW_CLOCK_FOLLOW` — the flip-on-or-delete commitment

This knob ships default-off because it changes real-time timing behaviour and
merging `main` auto-deploys to the live master rig, so it cannot be proven
without hardware. It is **not** an open-ended option: it is either turned on by
default once the rig test below passes, or the whole clock-discipline path is
deleted. It does not get to sit inert indefinitely — that is exactly how
`REACPW_EST_COMMIT`, `REACPW_ANNOUNCE_BURST`, `REAC_TX_LAYOUT` and
`REACPW_ANNOUNCE_UNGRANTED` rotted before being removed.

The rig procedure that decides it:

1. **Graph clock (the expected configuration).** With the RME as the elected
   PipeWire driver and reac-pw as REAC master to a real box, run with
   `REACPW_CLOCK_FOLLOW=1`. The transcript must name the device
   (`locked to graph clock (...)`), the applied correction must settle, and the
   box must stay `ESTABLISHED` with zero `ESTABLISHED -> PROBING` drops over a
   long run.
2. **Deliberate offset — converges without a phase step.** Offset the host clock
   against the reference and confirm the cadence converges as a slow pull: the
   applied ppm moves in bounded steps (never more than the slew limit per
   update), the FSM never drops, and there is no click. A phase step would be
   audible; the loop only ever changes the period's length.
3. **An external clock master is present — we follow it.** With the reference
   word-clocked from the house clock, confirm the applied correction tracks it
   rather than sitting at zero, and that reac-pw reports the reference it is
   following.
4. **Stagebox as the clock master.** With a box that is itself clock master and
   no PHC/hardware graph clock, confirm the reference reported is the box
   counter slope and the box stays established. This is the case where reac-pw
   is the REAC master while being a clock follower.
5. **Honest degradation.** Remove the reference mid-run: the transcript must say
   `holdover: reference lost, holding the last good rate`, the cadence must not
   snap, and it must never keep claiming lock.
6. **Quality is reported, and an unsuitable reference is refused (#77).** With
   the RME present the clock line must name the device *and* a tier; with
   `REACPW_CLOCK_REF=Babyface` set it must read `operator-designated`. Then
   remove the RME from the graph so an HDMI sink is the elected driver: reac-pw
   must fall through to the box counter slope (or to an honest free-run) and
   must never print `locked to graph clock` for the display device.
7. **Goldens unmoved.** `meson test -C build` green throughout, with no golden,
   fixture or assertion change — this changes WHEN frames go out, not what is in
   them.

Offline, the whole decision core is already pinned by `test_reac_clock` and the
wiring (including the inertness proof) by `test_reac_pacer_clock`.

## `REACPW_GRANT_ON_DECLARE` is the DEFAULT (ruled 2026-08-31, spec §3c)

**The dwell is a CAP for a box that has not declared, not a wait.** We do not wait for what the
box has already confirmed. It earns 1.55 s on both boxes we own — S-1608 1.684 s -> 0.134 s,
S-4000S 1.756 s -> 0.206 s — each audio-verified through a physical loopback, which is the
acceptance criterion: the box ends up enrolled and passing signal.

Set `REACPW_GRANT_ON_DECLARE=0` to opt out and restore the full wall-clock hold.

**Why this is not the risky change.** A shorter CONSTANT (`REACPW_GRANT_DWELL_MS`) fires whether
or not the box is ready, so it breaks the box that needs a long hold. Ending on the declaration
frame cannot fire early by construction — the box has already said it is there. The two were
argued as one for a while; they are not the same mechanism.

### The residue: a declaration is not the whole ladder

`dwell_ends_now` keys on OUR ENROLL plus a 50 ms settle after the box declared. It does not read
the box's `op0403`/`TAG0100` **JOIN field**, which climbs `01 -> 05 -> 0d` — nothing in this
daemon parses it; it exists only in a comment.

So the settle is a small constant standing in for a confirmation we could read directly. The full
form of §3c is to advance **on the rung the box confirms**, which needs that field parsed and a
golden to pin it. Until then the constant is the approximation, and it is named here rather than
left to look like a measurement.
