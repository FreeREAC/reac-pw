# reac-pw

PipeWire-native Roland REAC endpoint. A `libpipewire-0.3` client that registers
the REAC fabric as nodes in the PipeWire graph instead of a fixed point-to-point
bridge. Once REAC is a graph node, every destination is a `pw-link` away: monitor
on a DAC, emit AES67 through `module-rtp-sink`, or record to a file — same node,
no new code per destination.

## What it is

The **RX source node** is a 40-channel `reac:capture` Audio/Source fed from a live
REAC wire (AF_PACKET, EtherType `0x8819`) or a pcap replay, decoded with the
proven plain-LE core and handed to PipeWire's adapter for channel-map,
format-convert and adaptive resample. The **TX sink node** (`reac:playback`) is a
working REAC **master**: it encodes the graph's PCM into the downstream broadcast,
clocks the wire from a SCHED_FIFO cadence pacer at a steady pps, and drives the
cdea/cfea JOIN/HOLD handshake so a real Roland stagebox slaves to it (see Status).

Everything REAC-specific is reused, not reinvented:

- **libreac** (`FreeREAC/libreac`, header `<reac/reac.h>`) — frame validate, the
  byte-14/15 counter, gap math, `reac_detect_rate_fd` / `reac_rate_snap`. Pulled
  as a meson subproject.
- **reac-aes67 core** (`FreeREAC/reac-aes67`, `src/`) — `reac_decode.c` (plain-LE
  sample-major `(s*40+ch)*3`, on-rig coherence 0.999), `reac_capture.c` (live
  AF_PACKET), `pcap_source.c` (classic pcap reader). Compiled straight in from a
  sibling checkout.

reac-pw itself is only the lock-free ring, the RX feeder, and the two PipeWire
nodes.

## Build and run

External deps are just PipeWire and SPA via pkg-config (Fedora: `pipewire-devel`),
plus pthreads and libm. libreac is fetched by the meson wrap; the reac-aes67 core
is read from a sibling checkout (`../reac-aes67-pub` by default — override with
`-Dreac_aes67=PATH`).

```
meson setup   build
meson compile -C build
meson test    -C build                              # unit tests, no PipeWire needed
./build/reac-pw --pcap capture.pcap --rate 48000    # offline replay
sudo ./build/reac-pw --live reac0 --tx reac0        # MASTER (default): a box slaves to us
sudo ./build/reac-pw --live reac0 --role slave --tx reac0   # SLAVE: we slave to a desk
```

REAC has no fixed master — any box can be the master. `--role master` (default)
makes openmixer the master (we drive the cdea/cfea handshake + own the clock; a
stagebox slaves to us). `--role slave` makes us a box slaved to an external master
(it drives the handshake + owns the clock; we lock to its cadence and return our
inputs upstream). `--rate` forces 44100/48000/96000; omit it to auto-detect from
packet cadence on a live wire. `--tx IFNAME` is the REAC TX NIC (the master's
downstream sink, or the slave's upstream-return + handshake socket; needs
`CAP_NET_RAW`, plus `CAP_SYS_NICE` for the master pacer's SCHED_FIFO). The slave
role requires `--tx`.

On the master, `--box MODEL[:NAME]` (`MODEL` = `s0808`, `s1608`, or `s4000s`)
declares the single box on this REAC segment, sizing and labelling
`reac:capture`/`reac:playback` to its real input/output width; the optional
`:NAME` overrides the node label (default the model name). `--name NAME` suffixes
the PipeWire node names (`reac-capture.NAME`, `reac-playback.NAME`) so one master
per REAC VLAN/segment can coexist in the same graph. (`--box` is master-only; a
slave's own width is `--box-channels`.)

## Node model

The REAC broadcast is always 40 ch × 12 samples × 3 B; the sample rate lives in
the packet rate (pps = rate/12), never on the wire.

- **`reac:capture` (source).** A `pw_filter` with 40 mono-F32 DSP output ports —
  exactly the ring's planar layout. A non-realtime feeder thread reads frames,
  validates and counter-stamps with libreac, decodes with the reac-aes67 core,
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
  `process()` encodes each 12-sample group with `reac_tx_build` and submits it to
  a lock-free TX frame ring (no syscall on the graph thread). A dedicated
  SCHED_FIFO pacer thread (mlockall, prio ~79, `clock_nanosleep` TIMER_ABSTIME)
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

## Status

- **RX source node** — implemented (pcap + live), follower clock with ppm
  tracking; offline-testable.
- **Lock-free ring** — implemented, unit-tested (round-trip, underrun, overrun);
  the test needs no PipeWire so CI can run it anywhere.
- **TX sink node (REAC master)** — implemented: `reac_tx` encoder (round-trips
  through the decode core to 24-bit ULP), the `reac_master` cdea/cfea JOIN/HOLD
  handshake (control blocks byte-match the captured M-5000 + checksum), and the
  `reac_pacer` SCHED_FIFO cadence pacer (8000 fps / 125 µs measured on the wire).
  Loopback PCM→REAC→PCM verified (a tone played into `reac:playback` reaches the
  wire FILLER audio). **Not yet verified: a real Roland desk linking** — no desk
  on the bench; built correct-by-construction against the captures. The
  hardware-verify gate (does `RCQ` go `establishing`→`established`, does audio
  reach the box) is in [DESIGN.md](DESIGN.md).
- **SLAVE role** (`--role slave`, `reac_slave` over `reac_ctrl`/`reac_fsm`) —
  implemented: we respond to an external master, lock to its cadence (the master
  owns the clock — no own pacer), RX its audio via `reac:capture`, and return our
  input channels upstream at the box's slots. The establishment + HOLD FSM is
  offline-tested from the captured master control kinds (`test_reac_slave`); the
  JOIN cold-connect bytes + a real link both ways are behind the slave hardware-
  verify gate in [DESIGN.md](DESIGN.md).
- **Role selection** (`--role master|slave`, `reac_role.h`) — default master
  preserves the original behaviour; parse + validation unit-tested.

Target: Fedora + PipeWire 1.4.

GPL-3.0-or-later. Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>.
