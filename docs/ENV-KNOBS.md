# Environment knobs

Every `REACPW_*` / `REAC_*` environment variable reac-pw reads, one line each — the
same list `reac-pw --help` prints. The contract for all of them: **unset = default
behavior, byte-identical on the wire.**

Per-segment keys (`REAC_ROLE_<segment>`, `REAC_SRC_MAC_<segment>`, ...) live in
`~/.config/reac-pw/reac-pw.env` and take precedence over the bare key, which is only
a floor. `--role`, `--rate`, `--src-mac` and the other command-line flags still win
over both when given.

| Knob | Effect | Default |
|---|---|---|
| `REAC_TX=IFNAME` | The REAC TX NIC for this segment (master: the downstream sink; slave: the upstream return + handshake socket). | the same interface as the live/RX NIC |
| `REAC_ROLE=master\|slave\|auto` / `REAC_ROLE_<segment>` | Which end of the desk↔stagebox pairing to present on a segment. `auto` listens first: a desk mastering the wire is joined as a slave, a wire with a box and no master is taken as master after a 3 s hunt and granted, a stagebox strapped to master is refused and logged. The per-segment key overrides the wire; the bare key is only a floor for a segment nobody has heard yet. | `auto` |
| `REAC_MIXER=m200\|m300\|m5000` | Master role: which desk name reac-pw logs as. Does not set the wire's pace-code byte — that comes from `--rate` alone, and grants are box-defined, so any box locks regardless of profile. | `m200` |
| `REAC_NAME=NAME` | PipeWire node suffix (`reac-capture.NAME`, `reac-playback.NAME`) so more than one segment can coexist in the graph. | the interface name |
| `REAC_HEADAMP="CH:PARAM:VALUE ..."` | Master role: the per-channel head-amp table the master re-asserts to the box (space or comma separated). `CH` is the head-amp channel, `PARAM` is `phantom`\|`pad`\|`sens`, `VALUE` is 0/1 for phantom/pad or a raw SENS code. | unset (nothing re-asserted) |
| `REAC_BOX_CHANNELS=N` | Slave role: our own declared input width (even, 2..40; 8 = S-0808, 16 = S-1608, 32 = S-4000S). Sets the cold-connect/upstream/heartbeat width. | 16 |
| `REAC_SRC_MAC=aa:bb:cc:dd:ee:ff` / `REAC_SRC_MAC_<segment>` | Our on-wire source MAC. | the TX NIC's own hardware address |
| `REAC_RATE=44100\|48000\|96000` | The REAC sample rate for a segment. A master defines the rate; a slave auto-detects it from the wire cadence. | 96000 (master); auto-detected (slave) |
| `REACPW_GRANT_ON_DECLARE=0` | Master role: opt out of ending the grant dwell as soon as the box declares itself, restoring the full wall-clock hold. The dwell is a cap for a box that has not declared, not a wait. | on (end on declare) |
| `REACPW_GRANT_DWELL_S=N` | Master role: the grant-dwell cap, in whole seconds. | built-in ~1.6 s (a real M-200 holds a cold box ~27 s) |
| `REAC_DEBUG=1` | Opt-in RX/source telemetry on stderr, roughly every 2 s: frame/dup/gap counters, ring fill, active channels. | unset (silent) |
| `REAC_IFACES_ALLOW_WIRELESS=ifname[,ifname...]\|*` | Autodetect: opt a wireless interface into the scan. Wi-Fi's jitter makes REAC unworkable without a repacer this project does not have, so use this only for a deliberate, informed exception. | unset (every wireless NIC excluded) |
| `REACPW_NO_ENROLL=1` | Master role: suppress the pre-grant ENROLL for a box whose width is already known. A rig-test switch, not a new default. | unset (ENROLL sent) |
| `REACPW_CLOCK_FOLLOW=0` | Master role: opt out of disciplining the TX cadence to the best available clock reference (NIC/external PHC > a hardware-driven PipeWire graph clock > the box's counter slope) and free-run on `CLOCK_MONOTONIC` instead. Following steers the period continuously, never phase-steps, prints the reference in use on every change, and free-runs (and says so) when no reference is available. | on (follow the best reference) |
| `REACPW_CLOCK_REF=<substring>` | Designate which device is the clock reference — a case-insensitive substring of its name (e.g. `Babyface`). A designated device outranks the name heuristic; it does not rescue a structurally unusable reference (an HDMI/DisplayPort sink, a software timer), and it does not outrank measured instability. Only consulted while following. | unset (nothing designated) |
| `REACPW_CATCHUP_MAX_SLOTS=<n>` | Master role: how many overslept pacer slots are repaid by staying on the deadline grid instead of re-basing the phase and losing them. `-1` disables repayment. | 4 |
| `REACPW_RATE_MATCH=1` | Master role: opt in to publishing `io_rate_match` on the sink, so PipeWire's resampler absorbs the residual graph/wire difference instead of the depth guard discarding it in bulk. | unset (off) |
| `REACPW_RT_PRIO=<1..99>` | `SCHED_FIFO` priority for the wire-clock threads (the master pacer, the slave upstream engine) — neither is part of the PipeWire graph. The built-in sits below the whole audio graph on purpose, so a late wire slot never preempts the cycle that fills its own ring. Raise it only on a host whose PipeWire graph runs elsewhere. A value outside 1..99 is refused. | built-in 45 |

## `REACPW_CLOCK_REF` — reference quality tiers

reac-pw grades the clock reference it finds on a five-step ladder — `unusable` <
`marginal` < `ungraded` < `good` < `operator-designated` — and prints the tier next
to the device name on every clock line.

1. **Structurally disqualified is sticky.** Software timers (`clock.system.*`,
   Dummy-Driver, Freewheel-Driver) and display-derived sinks (HDMI, DisplayPort)
   are `unusable`. Nothing lifts that.
2. **The name heuristic can only reject, never promote.** There is no allow-list of
   "good" vendors. Anything not disqualified is `ungraded` — usable, no opinion.
3. **`REACPW_CLOCK_REF` outranks the heuristic.** A device whose name contains the
   designated substring is `operator-designated`.
4. **Measurement outranks everything above it.** The clock loop's filtered residual
   has a variance: sufficiently steady over a long tracking run promotes a
   reference to `good`; measurably wandering demotes it to `marginal`, whatever the
   badge says.

A `marginal` reference is still followed and loudly flagged rather than ejected —
ejecting it mid-run would reset the stability measurement and flap a live segment
on an opinion formed too soon.

Every knob above is also in `reac-pw --help` (the `environment` section of
`usage()` in `src/main.c`); keep the code, `usage()` and this file in sync when a
knob is added or removed.
