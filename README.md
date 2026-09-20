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

**Enabling the service — automatic for one logged-in user, otherwise a manual, per-user step.** The RPM installs
`reac-pw.service` as a systemd **user** unit (`/usr/lib/systemd/user/reac-pw.service`
— a bare system unit has no `HOME` and cannot find `~/.config/reac-pw/` or the
operator's PipeWire socket). No package scriptlet enables it GLOBALLY, on
install, upgrade or otherwise (since 1.0.19 — `%post` used to call
`systemctl --no-reload preset --global reac-pw.service`, and `--global` is not
scoped to the user running `dnf`: it applied to every systemd **user** instance
on the host, present or future, including one `sudo` spawns for root. Measured
2026-09-18: `sudo dnf install reac-pw-1.0.18` started a second daemon under
root's user manager, which won the abstract segment-lock socket and locked the
console user's own daemon out — "the segment is held" and every box vanished
until the root instance was killed by hand).

The package does the one safe thing instead: after the transaction, if there is **exactly one** real interactive login session on the host (logind class
`user`, never root), it enables and starts the unit for that user through their own
manager. Nobody logged in, several users, root only, or a lingering user with no
login: it touches nothing. In those cases run this once, as the console user:

```
systemctl --user enable --now reac-pw
```

**A hand-installed copy shadows the packaged unit.** If `~/.config/systemd/user/
reac-pw.service` exists (from before the RPM shipped one, or from following an
older version of this doc), it takes priority over `/usr/lib/systemd/user/
reac-pw.service` and every future `dnf upgrade` will appear to change nothing —
remove it before enabling the packaged unit:

```
systemctl --user list-unit-files reac-pw.service       # should say /usr/lib/systemd/user
rm -f ~/.config/systemd/user/reac-pw.service
systemctl --user daemon-reload
systemctl --user enable --now reac-pw
```

A package never writes into `$HOME` to fix this for you — removing a hand-written
file has to be a deliberate, visible step, not a postinst side effect.

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

With no flags at all — the packaged shape — reac-pw works the rig out for itself:
every linked Ethernet interface is sniffed, the first REAC frame heard makes it a
segment, every VLAN sub-interface is an interface like any other, a tagged VLAN id
with no sub-interface is created or named in a refusal, and every segment's ROLE is
`auto`: a box mastering the wire is slave-joined, a desk mastering it is tapped, a
silent wire is mastered and flooded. Nothing in the environment can change a role.

To override ONE segment — a switch mirror port, a recorder, a NIC to leave alone —
write `~/.config/reac-pw/reac-pw.conf` by hand (`[segment IFNAME]` with `role =` or
`ignore = yes`); nothing generates that file. See
[packaging/reac-pw.conf.example](packaging/reac-pw.conf.example) and
[the spec](docs/design/specs/2026-09-16-segments-and-roles-are-autodetected.md).

Everything above the built-in default EXCEPT the role can also be set through a
layered environment/config lookup — the command line wins, then the process
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
  On underrun it emits silent filler to keep the link alive. The frame's egress
  instant is a `SO_TXTIME` launch time the kernel's ETF qdisc releases — the
  default since 2026-09-14, ~10x tighter than the thread's own wake, with the
  daemon installing and removing that qdisc itself (`REACPW_PACER=thread` opts
  out; libreac's `docs/ETF-PACING.md` has the measurements). It also exposes
  standard `SPA_PARAM_Props` volume/mute/channelVolumes, so `wpctl
  set-volume`, the desktop mixer and WirePlumber attenuate the box outputs
  like any other PipeWire sink. It also states its own transmit health as node
  properties — see [docs/HEALTH-TELEMETRY.md](docs/HEALTH-TELEMETRY.md).

Both nodes are also the whole control surface: a client reads node properties
and writes node params, with PipeWire session access as the only gate. The
segment's rate and role, the box badge, and the stagebox head-amp door — its
per-channel phantom/pad/sens params, the travel, what reac-pw is asserting and
why a write was refused — are one table in
[docs/NODE-PROPERTIES.md](docs/NODE-PROPERTIES.md). A GUI over exactly that
door: [reac-stageboxes](https://github.com/FreeREAC/reac-stageboxes), a
desktop app for the preamps.

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
container from `packaging/reac-pw.spec` and publishes it into the public dnf
tree at [freereac.github.io/rpm](https://freereac.github.io/rpm) (one repo,
`freereac.repo`, one GPG key) — the same tree libreac's own release-rpm.yml
publishes into beside it. It is `workflow_dispatch` only, never on push:

```
gh workflow run release-rpm.yml -f tag=v1.0.1 -f sign=false   # dry run, publishes nothing
gh workflow run release-rpm.yml -f tag=v1.0.1 -f sign=true    # signs and publishes to the public dnf tree
```

`tag` must already exist and match `v[0-9]*`. `sign` defaults to `false`,
which stops before the tree is touched — the assembled unsigned tree is still
attached to the run as an artifact for inspection. Building needs
`pkgconfig(libreac) >= 1.0.1` and `pkgconfig(libreac-transport) >= 1.0.1`,
resolved from the same public tree by installing its `freereac.repo` before
`dnf builddep` runs — so **libreac's own equivalent workflow must have
published there first**, or the build fails loudly and by name.

**Hand-publish fallback**, if the workflow cannot run (no runner, a secret
missing): build locally and run `packaging/publish-repo.sh --rpm-dir DIR
--out <checkout of freereac.github.io> --key-id A14B3E1E1F69EBF4`, then
commit and push `rpm/` from that checkout — the same script the workflow
calls, run by hand over the same tree.

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

REAC is not a published protocol; everything here was measured. The entry
points for the protocol itself are
[libreac's `docs/REAC-CONTROL-PLANE.md`](https://github.com/FreeREAC/libreac/blob/main/docs/REAC-CONTROL-PLANE.md)
and `reac-protocol`'s `spec/reac.ksy` / `wire-format.md`.
