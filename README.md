# reac-pw

PipeWire-native Roland REAC endpoint. A `libpipewire-0.3` client that puts the
REAC fabric into the PipeWire graph as nodes instead of a fixed point-to-point
bridge. Once REAC is a graph node, every destination is a `pw-link` away:
monitor on a DAC, emit AES67 through `module-rtp-sink`, or record to a file —
same node, no new code per destination. See [docs/OUTPUTS.md](docs/OUTPUTS.md)
for the routing recipes.

## What it is

The **RX source node**, `reac:capture`, is an `Audio/Source` fed from a live
REAC wire (AF_PACKET, EtherType `0x8819`) or a pcap replay. The **TX sink
node**, `reac:playback`, is a working REAC **master**: it encodes the graph's
PCM into the downstream broadcast, clocks the wire from a `SCHED_FIFO` cadence
pacer, and drives the JOIN/HOLD handshake so a real Roland stagebox slaves to
it. A `--role slave` build does the reverse: it locks to an external master's
cadence and returns its inputs upstream as a stagebox's mic channels.

reac-pw itself is only the PipeWire binding — the RX/TX nodes, head-amp/rate/
role properties, and `main()`. The REAC byte layout, the cdea/cfea control
plane, the master and slave establishment FSMs, and the transport underneath
them (sockets, the lock-free ring, the cadence pacer, interface/VLAN scanning)
all live in [libreac](https://github.com/FreeREAC/libreac) and
libreac-transport, both at `>= 1.0.1`; reac-pw links against them rather than
reimplementing any of it.

Target: Fedora + PipeWire 1.4.

## Install

**From a release.** Every tagged release attaches the built RPMs and the source
tarball:

```
gh release download v1.0.1 -R FreeREAC/reac-pw -p 'reac-pw-*.rpm' -p 'libreac-*.rpm' -p 'libreac-transport-*.rpm'
sudo dnf install ./libreac-*.rpm ./libreac-transport-*.rpm ./reac-pw-*.rpm
```

`reac-pw` needs `libreac >= 1.0.1` (the REAC control plane) and
`libreac-transport >= 1.0.1` (the sockets/pacer/RT-thread/VLAN transport) —
install all three from the same release. The RPM sets the file capabilities
the daemon needs (`cap_net_raw,cap_net_admin,cap_sys_nice`), so it runs without
root.

**From source.**

```
sudo dnf install meson ninja-build gcc pipewire-devel libreac-devel libreac-transport-devel
meson setup build && ninja -C build && meson test -C build
```

`libreac-transport-devel` is a hard requirement (no fallback). `libreac-devel`
is resolved the same way, or meson's wrap fetches and builds it as a
subproject when no system package is new enough.

## Run

```
meson setup   build
meson compile -C build
meson test    -C build                              # unit tests, no PipeWire needed
./build/reac-pw --pcap capture.pcap --rate 48000     # offline replay
sudo ./build/reac-pw                                 # the packaged shape: hears its segments
sudo ./build/reac-pw --live reac0 --tx reac0                       # master, pinned to one NIC
sudo ./build/reac-pw --live reac0 --role slave --tx reac0          # slave: we slave to a desk
```

**The packaged shape configures nothing.** With no flags, reac-pw sniffs every
linked Ethernet interface passively; the first REAC frame it hears turns that
interface into a segment. A desk mastering the segment is joined as a slave, a
segment with a box and no master is taken as master after a three-second hunt
and the box is granted, and a stagebox strapped to master mode is refused with
the remedy named. Nothing has to be configured for a normal box to appear.

**The master learns the box from the wire.** reac-pw starts knowing nothing,
probes, and waits — no box present is a normal state. When a box declares
itself, the master allocates its head-amp slots and sizes `reac:capture` /
`reac:playback` to its real input/output width. Swap the box and that
re-derives; unplug it and it is forgotten.

## Configure

The main flags: `--live IFNAME` (live capture, repeatable for several
segments), `--pcap FILE` (offline replay), `--role master|slave`, `--rate
44100|48000|96000` (a master defines the rate; a slave auto-detects it),
`--tx IFNAME` (the REAC TX NIC), `--box-channels N` / `--box-model
s1608|s0808|s4000s` (a slave's own declared width/identity), `--name NAME`
(node-name suffix, for more than one master on one graph), `--src-mac`, and
`--headamp CH:PARAM:VALUE` (a master's re-asserted head-amp table). Run
`reac-pw --help` for the full, current list.

Everything above the built-in default can also be set through a layered
environment/config lookup — the command line wins, then the process
environment, then a per-segment key, then `~/.config/reac-pw/reac-pw.env`,
then `~/.config/openmixer/reac.env` as the last resort. See
[docs/RATE-AND-CLOCK-CONFIG.md](docs/RATE-AND-CLOCK-CONFIG.md) for the full
precedence and [docs/ENV-KNOBS.md](docs/ENV-KNOBS.md) for every `REAC_*` /
`REACPW_*` variable, what it does, and its default.

On Fedora, setting up the REAC interface itself (VLAN, address-less link) is
covered in [docs/fedora-network.md](docs/fedora-network.md).

## Roles, and the recorder rule

`--role master` (the default) makes reac-pw drive the cdea/cfea handshake and
own the clock — a real stagebox slaves to it. `--role slave` makes reac-pw a
box slaved to an external master: the master drives the handshake and owns the
clock, and reac-pw locks to its cadence and returns its own inputs upstream —
this is the shape a recorder uses to capture a live desk's buses.

**The recorder rule.** A REAC segment holds one box session per slot. If
reac-pw is already a granted slave on a venue's segment when one of the
venue's own boxes power-cycles, the desk has no way to notice the real box is
back — it keeps feeding the session it already has, and the real box never
re-enrols. Run reac-pw's slave role only after the venue's own boxes are
already enrolled, and take it off the segment before a box that needs to
reboot mid-show is expected to come back.

## Node model

The REAC broadcast is always 40 ch × 12 samples × 3 B; the sample rate lives
in the packet rate, never on the wire.

- **`reac:capture` (source).** Mono-F32 output ports, sized to the recognized
  box's real input width. A non-realtime feeder thread reads, validates and
  decodes frames into a lock-free ring; the only realtime code dequeues one
  quantum per channel. The node defaults to a **follower** — the DAC/NIC clock
  drives the graph and PipeWire resamples REAC into it. Set it as the graph
  **driver** instead to run REAC as the master clock.
- **`reac:playback` (sink, the REAC master).** Mono-F32 input ports; a
  realtime `process()` encodes each 12-sample group and hands it to a
  lock-free TX ring. A dedicated `SCHED_FIFO` pacer thread emits one frame per
  slot at a fixed rate and drives the JOIN/HOLD handshake onto the broadcast.
  On underrun it emits silent filler to keep the link alive. It also exposes
  standard `SPA_PARAM_Props` volume/mute/channelVolumes, so `wpctl
  set-volume`, the desktop mixer and WirePlumber attenuate the box outputs
  like any other PipeWire sink. It also states its own transmit health as node
  properties — see [docs/HEALTH-TELEMETRY.md](docs/HEALTH-TELEMETRY.md).

## Tools and tests

`meson test -C build` runs the full unit suite; it needs no PipeWire and no
live REAC wire. `tools/` carries the on-wire diagnostics used to measure a
running daemon without a rebuild — among them `clock-drift.py` and
`ring-depth.sh` (pacer/clock health), `probe-ports.sh` and `wav-rms.py`
(per-port level), and `tone-purity.py` / `sine-level.py` (signal quality on a
captured tone).

## Known issues

A box that boots while reac-pw is a slave on its segment does not enrol with
the desk: reac-pw's slave keeps streaming while ungranted, and the desk keeps
one box session per segment, so it never notices the real box rebooted and
never reopens the slot. With reac-pw off the segment, the desk drops the
session in a few seconds and the box enrols. This is the failure the recorder
rule above exists to avoid.

## Releasing

`.github/workflows/release-rpm.yml` builds the reac-pw RPM in a `fedora:44`
container from `packaging/reac-pw.spec` and publishes it into the same shared
dnf tree FreeMixer/openmixer's own release publishes into (one repo, one
`openmixer.repo`, one GPG key). It is `workflow_dispatch` only, never on push:

```
gh workflow run release-rpm.yml -f tag=v1.0.1 -f sign=false   # dry run, publishes nothing
gh workflow run release-rpm.yml -f tag=v1.0.1 -f sign=true    # signs and pushes to the shared R2 bucket
```

`tag` must already exist and match `v[0-9]*`. `sign` defaults to `false`,
which stops before the push step — the assembled tree is still attached to the
run as an artifact for inspection. Building needs `pkgconfig(libreac) >=
1.0.1` and `pkgconfig(libreac-transport) >= 1.0.1` resolvable from the same
shared tree, so libreac's own equivalent publish must have landed there first.

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

REAC is not a published protocol; everything here was measured. The entry
points for the protocol itself are
[libreac's `docs/REAC-CONTROL-PLANE.md`](https://github.com/FreeREAC/libreac/blob/main/docs/REAC-CONTROL-PLANE.md)
and `reac-protocol`'s `spec/reac.ksy` / `wire-format.md`.
