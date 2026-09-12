# reac-pw

PipeWire-native Roland REAC endpoint. A `libpipewire-0.3` client that registers
the REAC fabric as nodes in the PipeWire graph instead of a fixed point-to-point
bridge. Once REAC is a graph node, every destination is a `pw-link` away: monitor
on a DAC, emit AES67 through `module-rtp-sink`, or record to a file — same node,
no new code per destination.

## What it is

The **RX source node** is a `reac:capture` Audio/Source fed from a live REAC wire
(AF_PACKET, EtherType `0x8819`) or a pcap replay and handed to PipeWire's adapter
for channel-map, format-convert and adaptive resample. Both RX directions decode
libreac's braid oracle — box returns (the master's RX), rig-proven on real
microphones, and the master's downstream broadcast, which since libreac 0.5.0
`reac_decode()` un-braids too, so the decoder now agrees with the encoder in the
same binary (issue #80). The **TX sink node** (`reac:playback`) is a
working REAC **master**: it encodes the graph's PCM into the downstream broadcast,
clocks the wire from a SCHED_FIFO cadence pacer at a steady pps, and drives the
cdea/cfea JOIN/HOLD handshake so a real Roland stagebox slaves to it (see Status).

Everything REAC-specific is reused, not reinvented:

- **libreac >= 0.5.0** (`FreeREAC/libreac`, headers `<reac/*.h>`) — the single
  REAC byte-layout oracle: frame validate, the byte-14/15 counter, gap math,
  `reac_detect_rate_fd` / `reac_rate_snap`, the braid codec
  (`<reac/reac_braid.h>`), the f32↔s24 sample pair (`<reac/reac_sample.h>`),
  the box-upstream decode (`<reac/reac_upstream.h>`), the OHRCA +2 length rule
  (`reac_frame_clean_len`), plus `reac_decode.c` (the downstream decode —
  braided since 0.5.0, with the old plain-LE layout kept only as the named
  diagnostic `reac_decode_plain_le()`), `reac_capture.c` (live AF_PACKET) and
  `pcap_source.c` (classic pcap reader). System `libreac-devel` via pkg-config,
  or the meson wrap fallback. The floor is 0.5.0 rather than 0.3.0 because a
  0.4.x libreac links fine and then decodes the downstream with the layout the
  encoder does not write (#80).

reac-pw itself is now only the PipeWire binding: the RX/TX nodes, head-amp/rate/role props, and
`main()`'s node half. The control plane (cdea/cfea and DT1 record builders + the two checksums,
the master and slave establishment FSMs, the grant/ENROLL sweep, the multi-box registry) moved to
libreac in 0.8.0; the transport underneath it (the lock-free ring, the RX feeder, the cadence
pacer and its clock discipline, interface/VLAN scanning, the segment lock) moved to
libreac-transport in 0.5.11 (`docs/design/specs/2026-09-11-reac-transport-library.md`, in the
libreac repo). See DESIGN.md's Files table.

## Build and run

External deps are just PipeWire and SPA via pkg-config (Fedora: `pipewire-devel`),
plus pthreads and libm. libreac resolves to the system `libreac-devel` when new
enough, else the meson wrap fetches and builds it as a subproject.

```
meson setup   build
meson compile -C build
meson test    -C build                              # unit tests, no PipeWire needed
./build/reac-pw --pcap capture.pcap --rate 48000    # offline replay
sudo ./build/reac-pw                                # the packaged shape: HEARS its segments on every linked NIC
sudo ./build/reac-pw --live reac0 --tx reac0        # MASTER (default) pinned to one NIC: a box slaves to us
sudo ./build/reac-pw --live reac0 --role slave --tx reac0   # SLAVE: we slave to a desk
```

**The packaged shape configures nothing at all.** With no flags, reac-pw sniffs every
linked Ethernet interface passively, and the first REAC frame it hears turns that
interface into a segment. It then takes the end of the pairing the wire leaves open:
a desk mastering the segment is JOINED as a slave, a segment with a box on it and no
master is TAKEN as master after a three-second hunt (three master announce cadences)
and the box is granted, and a stagebox strapped to master mode is REFUSED with the
remedy named and never fought. `REAC_ROLE_<segment>=master|slave` in
`~/.config/reac-pw/reac-pw.env` overrides that for one segment; a bare `REAC_ROLE` is
only a floor for a segment nobody has heard yet, and the hunt supersedes it out loud.
Nothing has to be written for a normal box to appear.

REAC has no fixed master — any box can be the master. `--role master` (default on a
pinned `--live` run)
makes openmixer the master (we drive the cdea/cfea handshake + own the clock; a
stagebox slaves to us). `--role slave` makes us a box slaved to an external master
(it drives the handshake + owns the clock; we lock to its cadence and return our
inputs upstream). `--rate` forces 44100/48000/96000; omit it to auto-detect from
packet cadence on a live wire. `--tx IFNAME` is the REAC TX NIC (the master's
downstream sink, or the slave's upstream-return + handshake socket; needs
`CAP_NET_RAW`, plus `CAP_SYS_NICE` for the master pacer's SCHED_FIFO). The slave
role requires `--tx`.

On the master, **the box is learned from the wire and nothing configures it.**
reac-pw starts knowing nothing, probes, and waits; no box present is a normal
state, not an error. When a box declares itself (its `cdea 01 03 0010`
config-announce, matched against the fixed model matrix) the master allocates its
head-amp slots, generates the enrollment sweep for exactly those slots, and sizes
and labels `reac:capture`/`reac:playback` to its real input/output width. Swap the
box and all of that is re-derived; unplug it and it is forgotten rather than left
standing as a claim about a box that has gone.

`--box` is **retired** (2026-08-05): accepted, ignored, and reported once, plus
once more if the wire disagrees with it. It declared a fact only the wire can
state, and two sources for one fact means nothing forces them to agree while only
one of them is ever true — the head-amp slot addresses of a box nobody had seen
came from a compile-time default. A SLAVE's `--box-channels` / `--box-model` stay:
that is our declaration about ourselves, and there is no wire to learn it from.

`--name NAME` suffixes the PipeWire node names (`reac-capture.NAME`,
`reac-playback.NAME`) so one master per REAC VLAN/segment can coexist in the same
graph.

## Node model

The REAC broadcast is always 40 ch × 12 samples × 3 B; the sample rate lives in
the packet rate (pps = rate/12), never on the wire.

- **`reac:capture` (source).** A `pw_filter` with mono-F32 DSP output ports —
  exactly the ring's planar layout, sized to the recognized box's real input width
  (40 wide only where there is no recognizer: a pcap replay or a TX-less
  monitor). A non-realtime feeder thread reads frames,
  validates, counter-stamps and decodes with libreac,
  and writes whole REAC frames into a lock-free SPSC ring. The only realtime code
  is `on_process()`: it dequeues one PipeWire quantum per channel and returns —
  no format or rate conversion, that's the adapter on each outgoing link.
  Underrun zero-fills (the DAC never gets garbage); steady drift is the
  resampler's job. The node defaults to a **follower**: the DAC/NIC clock drives
  the graph and PipeWire async-resamples REAC↔graph, with the feeder publishing a
  filtered ppm error (counter slope vs `CLOCK_MONOTONIC`) into the port's
  `SPA_IO_RateMatch`. Setting it as the graph **driver** instead runs REAC as the
  master clock and async-resamples the DAC to it. Same node, only the driver flag
  + clock registration differ.
- **`reac:playback` (sink, the REAC master).** N mono-F32 input ports; the RT
  `process()` encodes each 12-sample group with libreac's `reac_downstream_build`
  and submits it to
  a lock-free TX frame ring (no syscall on the graph thread). A dedicated
  SCHED_FIFO pacer thread (mlockall, prio below the PipeWire graph — see
  `src/reac_rt.h` and `REACPW_RT_PRIO`, `clock_nanosleep` TIMER_ABSTIME)
  emits one frame per slot at a fixed pps (125 µs @96 k) and stamps the master
  JOIN/HOLD sequence — probe → `cdea 04 03` grant → established `cdea 01 03`
  channel-map + `cfea` announce ~1/s — onto the broadcast, so a real desk links.
  On underrun the pacer emits silent FILLER to keep cadence + link alive. The
  node also exposes standard `SPA_PARAM_Props` **volume / mute / channelVolumes**
  (with a channel map over the AUX ports), so `wpctl set-volume`, the desktop
  mixer and WirePlumber attenuate the box outputs; the raw filter has no
  audioadapter, so `process()` applies the gain itself while it stages samples,
  ramping toward the target to avoid zipper. Volumes are **linear** multipliers
  (1.0 = unity / 0 dBFS, 0.5 = −6 dB, 0.0 = silence), exactly as
  `SPA_PROP_channelVolumes` is defined and as PipeWire's own audioconvert applies
  them — so a REAC box sink behaves like a soundcard sink under standard controls.

See [DESIGN.md](DESIGN.md) for the data path, the clock topologies, and the TX
master handshake + pacer (S2/S6). This realizes `NATIVE-REAC-DESIGN.md` §3.4 (REAC
as pw-filter nodes, adaptive resample via `io_rate_match`).

## Releasing

`.github/workflows/release-rpm.yml` builds the reac-pw RPM in a `fedora:44`
container from `packaging/reac-pw.spec` and publishes it into the same shared
dnf tree FreeMixer/openmixer's own release publishes into (one repo, one
`openmixer.repo`, one GPG key — `dnf install openmixer-full` needs `reac-pw`
resolvable from it). It is `workflow_dispatch` only, never on push:

```
gh workflow run release-rpm.yml -f tag=v0.5.3 -f sign=false   # dry run, publishes nothing
gh workflow run release-rpm.yml -f tag=v0.5.3 -f sign=true    # signs and pushes to the shared R2 bucket
```

`tag` must already exist and match `v[0-9]*`. `sign` defaults to `false`, which
runs `packaging/publish-repo.sh --no-sign` and stops before the push step — the
assembled tree is still attached to the run as an artifact for inspection.
Building needs `pkgconfig(libreac) >= 0.9.0` and `pkgconfig(libreac-transport) >= 0.9.0`
resolvable from the same shared tree (`dnf builddep` against the spec), so libreac's own
equivalent publish must have landed there first.

## Status

- **RX source node** — implemented (pcap + live), follower clock with ppm
  tracking; offline-testable.
- **Lock-free ring** — implemented, unit-tested (round-trip, underrun, overrun);
  the test needs no PipeWire so CI can run it anywhere.
- **TX sink node (REAC master)** — implemented and **rig-verified**: `reac_tx`
  encoder (round-trips through the decode core to 24-bit ULP), the `reac_master`
  cdea/cfea JOIN/HOLD handshake (control blocks byte-match the captured M-200 /
  M-300 / M-5000 + checksum), and the `reac_pacer` SCHED_FIFO cadence pacer
  (125 µs @ 96 k / 250 µs @ 48 k measured on the wire). A real **S-0808** and a
  real **S-1608** cold-connect, are granted, reach ESTABLISHED and hold a 1/s
  heartbeat with zero drops; the box's mic channels reach `reac:capture` and run
  end-to-end through openmixer. See
  [docs/REAC-MIXER-PROTOCOL.md](docs/REAC-MIXER-PROTOCOL.md) (the five fixes that
  made the box lock SOLID) and
  [docs/MASTER-HARDWARE-VERIFY.md](docs/MASTER-HARDWARE-VERIFY.md) (the run, the
  observability, the remaining gaps). Still unverified on hardware: the 96 kHz
  OHRCA emit path, whose gate is in that same file.
- **SLAVE role** (`--role slave`, `reac_slave` over `reac_ctrl`/`reac_fsm`) —
  implemented: we respond to an external master, lock to its cadence (the master
  owns the clock — no own pacer), RX its audio via `reac:capture`, and return our
  input channels upstream at the box's slots. The establishment + HOLD FSM is
  offline-tested from the captured master control kinds (`test_reac_slave`), and a
  real, cold-booted **M-200** enrolled reac-pw as a 16-ch stagebox in its REAC
  menu and held the link across a 300 s run (2026-07-11). The **M-5000 (OHRCA)**
  still re-hunts after granting us — the open gap is the OHRCA established-state
  shape, not a clock wall. See
  [docs/REAC-BOX-STATE-DIAGRAM.md](docs/REAC-BOX-STATE-DIAGRAM.md) and
  [docs/SLAVE-EMULATION-SCOPE.md](docs/SLAVE-EMULATION-SCOPE.md).
- **Role selection** (`--role master|slave`, `reac_role.h`) — default master on a
  pinned `--live` run; parse + validation unit-tested.
- **Segment naming** (0.5.0) — a segment IS its interface and is NAMED after it, so the
  node pair is `reac-capture.enp131s0` / `reac-playback.enp131s0` and the per-segment conf
  key is `REAC_ROLE_enp131s0`; `<iface>.env` files are no longer read at all. Node names
  that follow the BOX instead are owed — see DESIGN.md's "what it owes". Later increments
  are 0.5.1, 0.5.2, 0.5.3, 0.5.4, ...; the middle digit does not move again for them.
- **Autodetect + role election** (0.5.0, `reac_ifscan` + `reac_hunt`) — the daemon
  finds its own segments (rtnetlink link state, a passive `0x8819` sniff) and elects
  its own role per segment from what it hears. Proven on a veth pair inside an
  unprivileged namespace, whole-binary: a desk on the peer end is joined as a slave,
  and a box on a vacant wire is taken as master, granted, and reaches ESTABLISHED —
  with an empty `$HOME` and no arguments (`tests/hearing-finds-a-segment.sh`).
- **Trunk topology** (0.5.3, `reac_topo` + `reac_vlan`) — on a trunk port the daemon hears
  the 802.1Q tags on each physical parent (a read-only `ETH_P_ALL` tap reading
  `PACKET_AUXDATA`, the only socket that can tell a tagged frame from an untagged one) and
  makes the sub-interface each VLAN needs: `<parent>.<vid>` ADOPTED where the host already
  created it, CREATED over rtnetlink and marked `reac-pw:minted` where it did not exist.
  Each is then an ordinary segment. What the daemon minted it removes on exit; what it
  adopted it leaves. **Capabilities: `cap_net_raw` for the sockets and `cap_net_admin` for
  creating, marking and removing those netdevs** (the RPM's `%caps` line grants both, and
  `tools/build.sh` re-applies them after every relink). Without `cap_net_admin` the daemon
  names every VLAN it cannot serve and goes on hearing — adoption needs no capability.
  Proven on veth, whole-binary: two VIDs on one wire, two segments, two boxes.
- **Re-resolution after a segment is up** (0.5.4, `reac_watch`) — a wire we took and
  nobody pinned keeps its passive sniffer, so a desk that is powered up AFTER the console
  is yielded to (master down, slave up, following its pace) and, when it goes home again,
  the wire is taken back. A pinned segment keeps its role. The segment's `reac.pace.source`
  says what the pacer is actually disciplined to — `phc`, `graph-ref` or `box-slope` while
  the DLL is locked, `free-run` while acquiring or in holdover, `foreign-master` where
  somebody else times the wire — instead of the constant it published until now.
- **Clock discipline** (0.5.0, `reac_clock`) — ON by default: the TX cadence follows
  the best available reference (NIC/external PHC > a hardware-driven graph clock >
  the box's counter slope), the period is slewed and never phase-stepped, and with no
  reference the pacer free-runs and says so. `REACPW_CLOCK_FOLLOW=0` opts out.

Target: Fedora + PipeWire 1.4.

GPL-3.0-or-later. Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>.

## Known issues

- **A box that boots while reac-pw is a slave on its segment does not enrol with the desk.**
  Measured 2026-09-11 with an M-200 and an S-1608 at 44.1 kHz, four trials. reac-pw's slave
  announces and streams as a 16-channel box and keeps streaming while ungranted; the desk keeps
  one box session per segment, fed by any upstream of that geometry, so it never notices the
  real box rebooted and never reopens the slot. With reac-pw off the segment the desk drops the
  session in about seven seconds and the box enrols. 1.0.1 bounds an ungranted slave's
  courtship (4 s on, 10 s off) and that is all it does: on the rig (2026-09-12) the desk granted
  the courting slave while its box was away, and the box was blocked just the same. The rule for
  recording a desk's segment stands: the venue's boxes enrol first, reac-pw joins last, and a
  box that power-cycles mid-show needs reac-pw off its segment to come back.

## Install

**From a release.** Every tagged release attaches the built RPMs and the source tarball:

```
gh release download v0.5.7 -R FreeREAC/reac-pw -p 'reac-pw-*.rpm' -p 'libreac-*.rpm' -p 'libreac-transport-*.rpm'
sudo dnf install ./libreac-*.rpm ./libreac-transport-*.rpm ./reac-pw-*.rpm
```

`reac-pw` needs `libreac >= 0.9.0`, which carries the REAC control plane, and
`libreac-transport >= 0.9.0`, which carries the sockets/pacer/RT-thread/VLAN transport
(`docs/design/specs/2026-09-11-reac-transport-library.md`, in the libreac repo); install all
three from the same release. The RPM sets the file capabilities the daemon needs
(`cap_net_raw,cap_net_admin,cap_sys_nice`), so it runs without root — a library cannot hold a
capability, so this package keeps them and the linked transport code runs inside this process.

**From source.**

```
sudo dnf install meson ninja-build gcc pipewire-devel libreac-devel
meson setup build && ninja -C build && meson test -C build
```

Run it with no arguments: it finds its segments by hearing them.

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

## The protocol

REAC is not a published protocol; everything here was measured. The entry points are
[libreac's `docs/REAC-CONTROL-PLANE.md`](https://github.com/FreeREAC/libreac/blob/main/docs/REAC-CONTROL-PLANE.md)
for how endpoints pair, and `reac-protocol`'s `spec/reac.ksy` and `wire-format.md` for the
frames themselves. Where a document and a capture disagree, the capture wins and the document
is amended with the date and the evidence.
