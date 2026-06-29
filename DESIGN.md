# reac-pw — PipeWire-native REAC endpoint

A libpipewire-0.3 client that puts a Roland REAC fabric into the PipeWire graph
as first-class nodes. The **RX source node** (the monitor box) is a 40-channel
`reac:capture` Audio/Source fed from a live REAC wire or a pcap replay. The **TX
sink node** (`reac:playback`) is a working REAC **master**: it encodes the
graph's PCM into the downstream broadcast, clocks the wire from a SCHED_FIFO
cadence pacer, and drives the cdea/cfea JOIN/HOLD handshake so a real Roland
stagebox slaves to it (see the TX-path section, S2/S6).

Realizes NATIVE-REAC-DESIGN.md §3.4 (REAC as pw-filter nodes, adaptive resample
via `io_rate_match`, follower vs driver clock topologies). Target: Fedora +
PipeWire 1.4.

Reuses, does not reinvent:

- **libreac** (`FreeREAC/libreac`, `<reac/reac.h>`) — RX validate, the byte-14/15
  counter, gap math, `reac_detect_rate_fd` / `reac_rate_snap`. Pulled as a meson
  subproject.
- **reac-aes67 core** (`FreeREAC/reac-aes67`, `src/`) — `reac_decode.c` (plain-LE
  sample-major `(s*40+ch)*3`, on-rig coherence 0.999), `reac_capture.c`
  (AF_PACKET 0x8819), `pcap_source.c` (classic pcap reader). Compiled straight in
  from a sibling checkout.

Everything REAC-specific is borrowed. reac-pw itself is only: the lock-free ring,
the RX feeder, and the two PipeWire nodes.

## Data path (RX, Phase 1)

```
REAC NIC (AF_PACKET 0x8819)  ─┐
                              ├─► reac_rx feeder thread (SCHED_OTHER, producer)
pcap replay (offline test)  ─┘     reac_frame_is_reac / reac_frame_counter   [libreac]
                                   reac_decode → planar s24 → f32             [reac-aes67 core]
                                   reac_ring_write
                                        │  (lock-free SPSC ring, planar f32)
                                        ▼
                                   on_process()  (PipeWire RT callback)
                                   reac_ring_read_planar → 40 output ports
                                        │
                                        ▼
                                   PipeWire adapter per link:
                                   channel-map + format-convert + adaptive RESAMPLE
                                        │
                              ┌─────────┼───────────────┐
                              ▼         ▼               ▼
                          ALSA/DAC   module-rtp-sink   file sink
                          (monitor)  (AES67 out)       (record)
```

One node, every destination is a `pw-link` away. AES67 stops being the hub and
becomes one optional output.

## Node model

### Source node `reac:capture` (the monitor box) — `reac_source_node.c`

A `pw_filter` registered `media.class = Audio/Source`, with `REAC_MAX_CHANNELS`
(40) DSP output ports (mono F32 planar — exactly the ring's layout). The node
advertises `node.rate = 1/<recovered REAC rate>` from `reac_detect_rate_fd`.

`on_process()` is the only realtime code: it dequeues one quantum per channel
from the ring into the port buffers and returns. It never converts formats or
rates — PipeWire's adapter on each outgoing link does channel-map,
format-convert (S24→graph F32) and resample.

**Ports / format / rate.** 40 output ports, `32 bit float mono audio` DSP
format, rate = the recovered REAC rate (48000 default, auto-detected on live,
snapped from cadence by libreac). The downstream broadcast is always 40 ch × 12
samp; the rate lives in packet cadence (pps = rate/12), never on the wire.

**RX hot-path → process() via the ring.** The feeder (`reac_rx.c`, a plain
pthread, NOT SCHED_FIFO — it's the producer) reads frames, decodes with the
reac-aes67 core into planar f32, and `reac_ring_write`s whole REAC frames (40 ×
12). `on_process` `reac_ring_read_planar`s one PipeWire quantum. The ring is a
single-producer/single-consumer lock-free SPSC (`reac_ring.c`): two atomics,
no locks, no allocation on the hot path. Underrun → process() zero-fills and
bumps a counter (the DAC never gets garbage); transient overrun → the feeder
drops the oldest frame (steady drift is the resampler's job, not the ring's).
Ring depth ≈ 250 ms at the recovered rate — enough to swallow a WiFi tail spike
(~8 ms) without touching the ~200 ms TX-mute (which the counter free-runs
through; we never resync the media clock to a transmission gap).

**Clock: follower (default, Tier-A) vs driver.** Default the node is a
**follower** (`PW_FILTER_FLAG_RT_PROCESS`, no driver flag): the DAC or NIC PHC
drives the graph and PipeWire async-resamples REAC↔graph. The feeder is the
**rate authority** — it tracks the byte-14/15 counter slope against
`CLOCK_MONOTONIC` and publishes a slowly-filtered ppm error
(`rx->ppm_error_milli`, recomputed ~4×/s, locked to the long-term slope not
packet jitter). The source node feeds that into the port's `SPA_IO_RateMatch`
so PipeWire's resampler tracks the desk's true rate. **That is the Tier-A clock
bridge, PipeWire-provided.** To run REAC as the graph **driver** instead (a pure
monitor sample-locked to the desk, no RX resampling), set `PW_KEY_NODE_DRIVER`
and register a clock source at the recovered rate — the DAC is then
async-resampled *to* REAC. Both topologies are the same node; only the driver
flag + clock registration differ.

### Sink node `reac:playback` (the REAC master) — `reac_sink_node.c`

**Functional.** An Audio/Sink with N mono DSP input ports; `process()` encodes
each 12-sample group with `reac_tx_build` and submits it to the SCHED_FIFO
pacer, which clocks the wire at a fixed pps and drives the master JOIN/HOLD
handshake so a real Roland stagebox slaves to us. See the TX-path section (S2/S6)
below. (Despite the name it presents as the *master*, not a stagebox — the
downstream broadcast is the master's role; a true virtual-stagebox upstream is
the separate `reac_ctrl`/`reac_fsm` slave half.)

## Build (meson + pkg-config)

`meson.build` + `meson_options.txt`. External deps via pkg-config: only
`libpipewire-0.3` and `libspa-0.2` (Fedora `pipewire-devel`) plus `threads` and
optional `m`. No libpcap (the pcap reader is the dependency-free reac-aes67 one;
live capture is raw AF_PACKET).

- **libreac** is a **meson subproject** (`subprojects/libreac.wrap`, a `wrap-git`
  on `FreeREAC/libreac`). Upstream libreac ships only a hand Makefile, so the
  wrap drops a tiny `meson.build` into the checkout via `patch_directory`
  (`subprojects/packagefiles/libreac/meson.build`) that builds `src/reac.c` into
  a static lib and exposes `libreac_dep` with `<reac/reac.h>`. No upstream source
  is touched.
- **reac-aes67 decode/capture/pcap core** is compiled **straight in** from a
  sibling checkout (`-Dreac_aes67=../reac-aes67`, default `../reac-aes67`),
  exactly as reac-aes67's own Makefile compiles libreac's `reac.c` in. The three
  files are dependency-free and already on-rig-proven; forking them would violate
  "reuse, do not reinvent". meson errors with a clear message if the checkout is
  missing.

```
meson setup build -Dreac_aes67=../reac-aes67-pub
meson compile -C build
meson test    -C build            # runs test_reac_ring (no PipeWire needed)
./build/reac-pw --pcap capture.pcap --rate 48000
sudo ./build/reac-pw --live reac0           # needs CAP_NET_RAW
```

`test_reac_ring` is self-contained (ring only — no PipeWire, no libreac), so CI
can run it anywhere. The node code needs `pipewire-devel` installed to compile.

## TX path (built) — `reac:playback` is a working REAC MASTER

`reac:playback` now emits real REAC downstream and presents as the **master**, so
a Roland stagebox slaves to it. The path is: graph → `reac_tx_build` (encode) →
the TX frame ring → the SCHED_FIFO pacer (cadence + master handshake) → the wire.

### S2. Master-role JOIN/HOLD handshake (`src/reac_master.{h,c}`)

`reac_ctrl`/`reac_fsm` are the *slave* half (a virtual stagebox responding to a
real master). `reac_master` is the inverse: **we are the master**. A real desk
only links when it sees the master cycle `cdea 01` sub-states, then GRANT with a
`cdea 04 03` burst, then settle to steady `cdea 01 03 0019` channel-map + `cfea`
announce (REAC-PROTOCOL-AND-TESTS.md §13d, the gold WIRED reference). The earlier
cut sent a zero control block (= FILLER), which carries audio but is no grant —
no desk links to it.

`reac_master` is a **pure decision function** (no I/O): given its state + the next
frame's counter it returns which control block to stamp into the downstream frame,
and `reac_master_stamp()` writes that block (type `[16:18]` + control block
`[18:50]`) over the frame `reac_tx_build` produced, re-applying the cdea/cfea
checksum and leaving the audio + counter + `C2 EA` tail intact. The state machine:

```
IDLE ──box present──▶ PROBING ──~1 s──▶ GRANTING ──~150 ms──▶ ESTABLISHED
  ▲                   (cdea 01 ss        (cdea 04 03           (FILLER audio +
  └──box absent──────  03→01→00→02)       grant burst)          cdea 01 03 0019
                                                                 chanmap + cfea ~1/s)
```

**Byte source-of-truth.** The control blocks are the EXACT 32-byte blocks
captured off the real M-5000 (reac-captures/wired-reac-a-bothdirs-2026-06-09,
master `00:40:ab:ca:15:4d`): the 6 established channel-map frames spanning the
full 40-ch walk, the `cfea` announce (inCh `0x28`=40, outCh `0x10`=16), plus the
`cdea 01` probe and `cdea 04 03` grant from the §6/§13d transcription.
`Sum(block[18..49]) mod 256 == 0` holds on every one (verified). We replay each
block verbatim and re-stamp only the live counter, so the bytes a desk sees match
the M-5000 by construction. The probe varies byte `[19]` through the sub-state
cycle `03→01→00→02` and re-applies the checksum; the chanmap cursor walks the 6
captured frames (never repeating within a cycle — dodges the desk's 6-identical
stale-guard). Channel `0x13` falls in a 7th map frame the 120 s capture missed
(coverage 47/48 slots) — a fidelity gap, not a link blocker.

The cdea/cfea control frames ride the 8000 fps broadcast **in-band**, occupying
audio slots ~1/s exactly as the real master does (§9 reac-repacer note: control
frames replace audio slots, never add to the stream).

### S6. SCHED_FIFO cadence pacer (`src/reac_pacer.{h,c}`)

A REAC slave recovers its word clock from the master's frame inter-arrival
interval, so the downstream broadcast MUST leave at a rock-steady pps or the box
hears rate jitter and drops the link. The PipeWire graph thread can't guarantee
that (bursty quantum + a `sendto()` syscall on the RT thread = wake jitter). So
emission moves to a dedicated thread, the **reac_repacer.c recipe**: `mlockall`,
`SCHED_FIFO` prio ~79, CPU-pinnable, woken every slot period by
`clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` on an absolute deadline that
advances by `period_ns` each tick (125.0/250.0/272.1 µs; no drift accumulation,
snap-forward on a late wake so we never burst-catch-up).

`on_process()` (the RT graph callback) only **encodes + submits**: it builds whole
1492-B frames with `reac_tx_build` and pushes them into a lock-free SPSC frame
ring — no syscall, no blocking. The pacer pops exactly one frame per slot; on
underrun it emits a **silent FILLER** so the cadence, the free-running counter and
the master heartbeat/channel-map never stall (the box stays locked without
clicks). The pacer asks `reac_master` what each outgoing frame carries and stamps
the counter + control block on egress, so the establishment sequence is
authoritative even across a graph stall. Here the wire is the rate authority: the
pacer drains at a fixed pps and PipeWire resamples the *graph* into it.

### Hardware-verify gate (a real Roland stagebox on the bench)

Built correct-by-construction against the design + captures — **a real desk
linking cannot be verified here** (no desk). When one is on the bench, capture at
the master side (`ether proto 0x8819`, both directions, ≥120 s) and confirm:

1. **`RCQ` / the desk's link state goes `establishing` → `established`** (the
   mixer UI flips from "linking" to a solid REAC connection / green light) while
   reac-pw is the only master on the segment.
2. **The desk stops its presence-flood and switches to linked unicast** the
   instant our `cdea 04 03` grant lands (§13d step 4) — confirms the grant bytes
   are accepted, not just well-formed.
3. **Audio reaches the box** — a tone played into `reac:playback` comes out the
   stagebox's analog outs at the mapped channel (proves the downstream FILLER
   audio + the channel-map the desk locked are mutually consistent).
4. **HOLD** — the link survives ≥5 min with no spurious drop (the master heartbeat
   + channel-map cadence keeps the desk's 600-frame loop-check fed).

That single capture also converts the firmware/rig-grade items to captured: the
exact grant-burst bytes, the per-rate cadence, the TX-mute dwell, and channel
`0x13`'s map frame. **Commit it to `reac-captures`, do not leave it in `/tmp`.**

Two rig-only unknowns remain open (do not guess): the box's upstream return-audio
FPGA channel-scramble (task #61 — a fidelity item, not a JOIN blocker) and the
4th, uncharacterised M-5000-internal HOLD-drop trigger (REAC-CONNECTION-FSM.md
gap list).

## Files

| File | Role |
|---|---|
| `src/main.c` | CLI + lifecycle: open feeder, create source node, (optional) create the master sink, run the loop |
| `src/reac_ring.{h,c}` | lock-free SPSC planar-float ring (RX hot-path → process()) |
| `src/reac_rx.{h,c}` | non-RT feeder: wire source (live/pcap) → libreac validate → reac-aes67 decode → f32 → ring; counter-slope ppm estimator |
| `src/reac_source_node.{h,c}` | `reac:capture` pw_filter: 40 F32 ports, RT process(), follower/driver clock |
| `src/reac_tx.{h,c}` | downstream-frame encoder (`reac_tx_build`, inverse of the decode core) + raw-socket emitter |
| `src/reac_master.{h,c}` | **master-role** JOIN/HOLD: the cdea/cfea establishment FSM + captured control-block templates (S2) |
| `src/reac_pacer.{h,c}` | **SCHED_FIFO cadence pacer** + TX frame ring; stamps the master block on egress (S6) |
| `src/reac_sink_node.{h,c}` | `reac:playback` Audio/Sink: process() encodes + submits to the pacer (the master TX) |
| `src/reac_ctrl.{h,c}`, `src/reac_fsm.{h,c}` | the SLAVE half (virtual-stagebox builders/parser/checksum + JOIN/HOLD FSM) |
| `tests/test_reac_ring.c` | SPSC ring unit test (round-trip, underrun, overrun) |
| `tests/test_reac_tx.c` | TX encoder ↔ decode-core round-trip (24-bit ULP, no channel cross-wire) |
| `tests/test_reac_master.c` | master cdea/cfea blocks byte-match the captures + checksum + chanmap walk + FSM sequence |
| `tests/test_reac_pacer.c` | pacer slot period (125/250/272 µs) + SPSC frame ring + live ~8000 fps emit (needs CAP_NET_RAW) |
| `meson.build`, `meson_options.txt` | build: pipewire/spa via pkg-config, libreac subproject, reac-aes67 core sibling |
| `subprojects/libreac.wrap` + `packagefiles/libreac/meson.build` | libreac as a meson subproject |
