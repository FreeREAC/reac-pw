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
| `REAC_DEBUG` | Opt-in diagnostic telemetry on stderr, ~every 2 s: RX feeder counters (ok/dup/other/bad/gaps, locked box MAC) in `reac_rx.c` and source-node ring stats (active channels, peak, fill) in `reac_source_node.c`. Set to any value to enable. | unset (silent) |
| `REACPW_CLOCK_FOLLOW` | Master role: DISCIPLINE the TX cadence to a clock reference instead of free-running on `CLOCK_MONOTONIC` (issue #75). Best available wins: NIC/external PHC > the PipeWire graph clock when driven by locked hardware > the box's counter slope. The slot period is steered continuously by a bounded DLL and the phase is never stepped; the reference in use is printed on every change, and with none available reac-pw free-runs and says so. Unset = the pacer advances its deadline by the fixed nominal period exactly as before — the discipline is never consulted, so emission and timing are identical. **RIG-GATED — see below.** | unset (free-run) |

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
6. **Goldens unmoved.** `meson test -C build` green throughout, with no golden,
   fixture or assertion change — this changes WHEN frames go out, not what is in
   them.

Offline, the whole decision core is already pinned by `test_reac_clock` and the
wiring (including the inertness proof) by `test_reac_pacer_clock`.
