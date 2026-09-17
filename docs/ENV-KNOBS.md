# Environment knobs

Every `REACPW_*` / `REAC_*` environment variable reac-pw reads, one line each — the
same list `reac-pw --help` prints. The contract for all of them: **unset = default
behavior, byte-identical on the wire.**

These configure a segment at start-up. What drives it while it runs is the node
properties and params in [NODE-PROPERTIES.md](NODE-PROPERTIES.md) — the rate, the role
and the stagebox head-amp door.

Per-segment keys (`REAC_RATE_<segment>`, `REAC_SRC_MAC_<segment>`, ...) live in
`~/.config/reac-pw/reac-pw.env` and take precedence over the bare key, which is only
a floor. `--rate`, `--src-mac` and the other command-line flags still win over both
when given.

## The ROLE is not here, and neither is a VLAN declaration

**`REAC_ROLE` and `REAC_ROLE_<segment>` are RETIRED** (2026-09-16). A segment's role is
autodetected from the wire, and the one thing that overrides it is a hand-written
`~/.config/reac-pw/reac-pw.conf` — see
[`../docs/design/specs/2026-09-16-segments-and-roles-are-autodetected.md`](design/specs/2026-09-16-segments-and-roles-are-autodetected.md)
and `packaging/reac-pw.conf.example`. A `REAC_ROLE*` key left on disk is read only so the
daemon can NAME it as ignored at start; it decides nothing.

```ini
[segment enp131s0.11]
role = tap        # auto | master | slave | tap   (default: auto)

[segment enp131s0.13]
ignore = yes      # never sniffed, served or minted
```

## A VLAN segment is DECLARED by being named — in reac-pw.conf

A `[segment <parent>.<vid>]` section **declares that VLAN segment**. Naming it is the whole
act; a section with no keys at all is a declaration. (The old forms — a per-segment KEY whose
name split as `<parent>.<vid>`, and a `~/.config/reac-pw/<parent>.<vid>.env` file — are gone
with the role key: both declared a segment as a side effect of a role projection, so the
declaration outlived what declared it.)

Every declared segment's `<parent>.<vid>` netdev is created and brought up **at start**,
and again whenever its parent appears — the daemon may start before NetworkManager has
brought the trunk up. It carries the `reac-pw:minted` interface alias, so a clean exit
removes exactly what it created and a netdev the host made is adopted and left untouched.

This is not the same path as the trunk detector, and it exists because that path cannot
cover a cold boot: `reac_topo` mints a VLAN when it HEARS a tagged REAC frame, every box
on a trunk is a slave, a slave says nothing until a master speaks, and the master cannot
speak until its segment's netdev exists. A VLAN that is heard and not declared keeps that
behaviour unchanged.

| Knob | Effect | Default |
|---|---|---|
| `REAC_TX=IFNAME` | The REAC TX NIC for this segment (master: the downstream sink; slave: the upstream return + handshake socket). | the same interface as the live/RX NIC |
| `REAC_ROLE` / `REAC_ROLE_<segment>` | **RETIRED (2026-09-16).** Read only to be NAMED at start as ignored. Use `reac-pw.conf`'s `[segment <name>] role =` above. | — |
| `REAC_MIXER=m200\|m300\|m5000` | Master role: which desk name reac-pw logs as. Does not set the wire's pace-code byte — that comes from `--rate` alone, and grants are box-defined, so any box locks regardless of profile. | `m200` |
| `REAC_NAME=NAME` | PipeWire node suffix (`reac-capture.NAME`, `reac-playback.NAME`) so more than one segment can coexist in the graph. | the interface name |
| `REAC_HEADAMP="CH:PARAM:VALUE ..."` | Master role: the per-channel head-amp table the master re-asserts to the box (space or comma separated). `CH` is the head-amp channel, `PARAM` is `phantom`\|`pad`\|`sens`, `VALUE` is 0/1 for phantom/pad or a raw SENS code. | unset (nothing re-asserted) |
| `REAC_BOX_CHANNELS=N` | Slave role: our own declared input width (even, 2..40; 8 = S-0808, 16 = S-1608, 32 = S-4000S). Sets the cold-connect/upstream/heartbeat width. **IGNORED under `role = box`** (2026-09-17 spec §2a, §5): a box row's `in_ch`/`out_ch` are its only width, and the key is read there only to be NAMED as ignored at start. | 16 |
| `REAC_SRC_MAC=aa:bb:cc:dd:ee:ff` / `REAC_SRC_MAC_<segment>` | Our on-wire source MAC. | the TX NIC's own hardware address |
| `REAC_RATE=44100\|48000\|96000` | The REAC sample rate for a segment. A master defines the rate; a slave auto-detects it from the wire cadence. | 96000 (master); auto-detected (slave) |
| `REACPW_GRANT_ON_DECLARE=0` | Master role: opt out of ending the grant dwell as soon as the box declares itself, restoring the full wall-clock hold. The dwell is a cap for a box that has not declared, not a wait. | on (end on declare) |
| `REACPW_GRANT_DWELL_S=N` | Master role: the grant-dwell cap, in whole seconds. | built-in ~1.6 s (a real M-200 holds a cold box ~27 s) |
| `REACPW_GRANT_DWELL_MS=N` | Master role: the grant-dwell cap, in milliseconds — finer-grained than `REACPW_GRANT_DWELL_S`. Whichever is set wins; unset both and the built-in applies. | built-in ~1.6 s |
| `REACPW_EST_SCENE=1` | Master role, rig falsification switch: stream a scene push into every FILLER slot the locked cadence leaves after the 1/s cfea and chanmap heartbeats keep theirs, to test whether that (not the cadence lock) is what a slow box needs. | unset (no added scene pushes) |
| `REACPW_BOX_MASTER_FRAME=box` | Box-master role, rig experiment: imitate the S-1608's own frame exactly (340 B at the master's width, unicast) instead of the ruling's 40-ch mixer frame. Any other value (or unset) is the mixer frame. | unset (40-ch mixer frame) |
| `REACPW_BOX_MASTER_BURST=chanmap` | Box-master role, rig experiment: burst the chanmap section instead of the default cadence. | unset (default cadence) |
| `REACPW_BOX_MASTER_FILL=noise` | Box-master role, rig experiment: fill unused slots with noise instead of silence. | unset (silence) |
| `REACPW_BOX_MASTER_PRESILENCE_MS=N` | Box-master role, rig experiment: milliseconds of silence to send before the first real frame. | 0 |
| `REACPW_GUARD_FLOOR_FRAMES=N` | libreac-transport's pacer: the ring-depth guard floor, in frames, for the M5 depth sweep — lets a sweep step change the floor without a rebuild. Out of range or unparseable keeps the compiled floor and says so. | the compiled `REAC_PACER_GUARD_FLOOR_FRAMES` |
| `REACPW_NO_HEADAMP=1` | libreac-transport's pacer: suppress our own head-amp push on establishment, so a box's own state-4 commit promotion stays visible instead of being overwritten ~1.7 s later. A diagnostic affordance, never a service mode. | unset (head-amp armed normally) |
| `REAC_DEBUG=1` | Opt-in RX/source telemetry on stderr, roughly every 2 s: frame/dup/gap counters, ring fill, active channels. | unset (silent) |
| `REAC_IFACES_ALLOW_WIRELESS=ifname[,ifname...]\|*` | Autodetect: opt a wireless interface into the scan. Wi-Fi's jitter makes REAC unworkable without a repacer this project does not have, so use this only for a deliberate, informed exception. | unset (every wireless NIC excluded) |
| `REACPW_NO_ENROLL=1` | Master role: suppress the pre-grant ENROLL for a box whose width is already known. A rig-test switch, not a new default. | unset (ENROLL sent) |
| `REACPW_CLOCK_FOLLOW=0` | Master role: opt out of disciplining the TX cadence to the best available clock reference (NIC/external PHC > a hardware-driven PipeWire graph clock > the box's counter slope) and free-run on `CLOCK_MONOTONIC` instead. Following steers the period continuously, never phase-steps, prints the reference in use on every change, and free-runs (and says so) when no reference is available. | on (follow the best reference) |
| `REACPW_CLOCK_REF=<substring>` | Designate which device is the clock reference — a case-insensitive substring of its name (e.g. `Babyface`). A designated device outranks the name heuristic; it does not rescue a structurally unusable reference (an HDMI/DisplayPort sink, a software timer), and it does not outrank measured instability. Only consulted while following. | unset (nothing designated) |
| `REACPW_PACER=etf\|thread` | Master role: which backend owns the instant a frame leaves. **`etf` is the default** (operator ruling, 2026-09-14): a `SCM_TXTIME` launch time on every frame off an exact `CLOCK_TAI` grid, released by the kernel's ETF qdisc, so the thread only has to be EARLY rather than punctual — measured on the TX device at ~10x tighter interval spread (sd 28.5 → 2.7 µs) with no rise in the daemon's own CPU. `thread` is the opt-out: `clock_nanosleep` + `sendto`, the egress instant being this thread's wake. **The daemon installs and removes the qdisc itself**, on the device it binds, over rtnetlink — no `tc` line on a rig, and a thread-backend start REMOVES a leftover `etf` root that would otherwise drop every frame it sends. An ETF precondition this machine cannot meet (no `sch_etf`, no `CAP_NET_ADMIN`, a zero kernel TAI offset) makes the DEFAULT log one loud line naming the errno and its fix, run the thread backend, and publish the refusal in `reac.pace.backend-refusal`; an EXPLICIT `REACPW_PACER=etf` still refuses to open instead, because a run that believes it is measuring launch-time pacing while the thread paces is worse than no run. Per-segment (`REACPW_PACER_<iface>`), so one segment can run each arm for a comparison. See libreac's `docs/ETF-PACING.md`. | `etf` |
| `REACPW_PACER_LEAD_US=<n>` | Master role, `etf` only: how far ahead of a frame's launch time the thread hands it to the kernel, in microseconds, 50..50000. The default is derived — 2000 µs of measured worst wake tail (this pacer's 30-minute soak) plus a 300 µs qdisc `delta` — not chosen. A lead is buffered audio, so longer is only more latency; shorter than the worst tail is a lead the thread will miss. It is also the BUDGET the health line's `wake-late` figure is spent against, and (less the qdisc `delta`) the ETF catch-up budget below. | 2500 |
| `REACPW_CATCHUP_MAX_SLOTS=<n>` | Master role: how many overslept pacer slots are repaid by staying on the grid instead of re-basing the phase and losing them. `-1` disables repayment. **The default is backend-dependent, because the two backends measure lateness against different references.** On `thread` it is 1000 µs of measured worst wake tail — 4 slots at 4000 fps, 8 at 8000. On `etf` it is the LEAD less the qdisc's 300 µs `delta`, because everything inside that is repayable by construction: the launch time is still in the future and the frame leaves exactly on the grid. With the shipped 2500 µs lead that is 17 slots at 8000 fps. Setting this keeps what you set, on either backend. | `thread`: 1000 µs; `etf`: the lead − 300 µs |
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

## Discovered AND published, not just documented (2026-09-17)

*"We should be able to set them and keep them if needed, and announce them when
detected, so that we can manage them; autodetection does not mean obscurity, it's
discovery and publish."* (operator ruling). At start, before any of them is acted
on, the daemon prints one line per knob that is SET — `src/reac_knobs.c`'s
`g_reac_knobs` table, the same one `tests/test_reac_knob_table.c` checks against
this file:

```
reac-pw: S_KNOB_SET knob REAC_DEBUG=1 (env)
reac-pw: S_KNOB_SET knob REACPW_PACER_LEAD_US=3000 (env)
reac-pw: S_KNOB_SUMMARY knobs: 2 set, 26 default
```

An unset knob prints nothing — silence is the whole story of a default. `(env)`
means the process environment answered; `(conf)` means `~/.config/reac-pw/reac-pw.env`
or `~/.config/openmixer/reac.env` did (`reac_conf_lookup`'s own layering,
`RATE-AND-CLOCK-CONFIG.md`). A knob marked `conf_capable` in the table is read
through that SAME lookup at its real call site — what is announced is what is
honoured. A knob that is not (today: `REACPW_CLOCK_REF`, forwarded verbatim into a
long-lived node that never copies it, and the libreac-internal knobs this lane
cannot verify without cutting a libreac release — `docs/design/specs/
2026-09-17-knobs-codes-and-test-ratchets.md` §6) is read with a bare `getenv` and
is announced the same way: never claimed to be layered when it is not.
`tests/test_reac_knob_table.c` fails when a knob the code reads is missing from
this file, or this file names one the code does not read.
