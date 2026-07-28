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

## Role: MASTER or SLAVE (`--role`, `src/reac_role.h`)

REAC has no fixed master — any box can be the master and the rest slave to it
(REAC-PROTOCOL-AND-TESTS.md §2/§4). openmixer must fit either role, selected at
launch with `--role master|slave` (default **master**, which preserves the
original behaviour). Both roles share the same encoder/decoder (`reac_tx_build` /
the reac-aes67 decode core) and the same PipeWire nodes (`reac:capture` for RX,
the sink for the graph's PCM). **They differ only in WHO drives the handshake +
the clock, and in the TX direction:**

| | **MASTER** (`--role master`, default) | **SLAVE** (`--role slave`) |
|---|---|---|
| **Handshake** | WE drive it: cycle `cdea 01` probe → `cdea 04 03` grant burst → steady `cdea 01 03` channel-map + `cfea` announce. A stagebox slaves to **us**. (`reac_master`, S2) | An EXTERNAL master drives it; WE RESPOND: flood broadcast FILLER → RX the master's probe → RX its `cdea 04 03` grant → settle → unicast our inputs up + a box heartbeat. (`reac_fsm` via `reac_slave`, S7) |
| **Clock** | WE own it: a dedicated SCHED_FIFO `clock_nanosleep` pacer emits the downstream at a rock-steady pps (8000/4000/3675); PipeWire resamples the *graph* into it. (`reac_pacer`, S6) | The MASTER owns it: we **lock to the incoming master cadence** — every received master frame is one slot tick and we emit exactly one upstream frame per tick. We never run our own pacer as the timing source. |
| **TX** | the **downstream broadcast** (40-ch program, dst `ff:ff:…`), encoded from the graph's PCM. | the **upstream return** (our box-width input channels, unicast to the learned master), placed at the box's slots — a slave sends its inputs INTO the stream. |
| **RX** | a box's upstream return — presence detection AND its input AUDIO: the box-width braided frame is decoded by `reac_upstream` (layout resolved from the captures, task #108). | the master's downstream audio, via the same `reac:capture` source node. |

The `--tx IFNAME` flag means "the REAC TX NIC" in both roles: the master's
downstream sink, or the slave's upstream-return + handshake socket. The slave
role REQUIRES `--tx` (it must have a NIC to answer on); the master role can run
RX-only (a pure monitor) or with `--tx` for the downstream sink.

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

`reac_master` is a **pure decision core** (no I/O): the pacer thread owns it,
feeds it classified RX events (`reac_master_rx`) and asks it once per emitted
frame what to stamp (`reac_master_next` + `reac_master_stamp`, which writes type
`[16:18]` + control block `[18:50]` over the frame `reac_tx_build` produced,
re-applying the cdea/cfea checksum and leaving audio + counter + `C2 EA` tail
intact).

**EVENT-DRIVEN establishment (task #130).** A real M-5000 never advances the
handshake on a timer — the earlier cut auto-advanced PROBING→GRANTING after ~1 s
and reached ESTABLISHED against a silent wire. Now every FORWARD transition is
gated on a received box control frame; the only timers left move BACKWARD to
PROBING (safety fallbacks):

```
              pacer starts emitting
IDLE ────────────────────────────────▶ PROBING  (FILLER + cdea 01 probes ~180/s,
                                          │       00-heavy ss cycle + cfea @1 Hz)
                 rx box JOIN (cdea 04 03, │       — presence alone NEVER grants —
                 validated cold-connect)  ▼
                                       GRANTING (ECHO the box's own JOIN block,
                                          │       1 grant / 12 slots, ~150 ms window)
                 rx first box UNICAST     ▼
                 (audio / hb / any)    ESTABLISHED (FILLER audio + chanmap @1/s
                                                    + cfea @1/s, phase-offset)

SAFETY FALLBACKS (backward only, with a typed drop reason + log line):
  GRANTING     ──window expiry, no unicast──▶ PROBING   (grant-timeout; the box
                                                          retries JOIN on ~100 ms)
  ESTABLISHED  ──600-frame budget drained──▶ PROBING    (peer-gone; every box RX
                                                          event reloads the budget)
  ESTABLISHED  ──box hb selector 0x00──────▶ PROBING    (explicit BYE)
  ESTABLISHED  ──JOIN from another MAC─────▶ GRANTING   (mac-change: re-latch +
                                                          re-court the new box)
```

The counter free-runs across every transition. There is no presence gate: the
box only emits its cold-connect on a real PHY link-down/up (§13b), so a master
that waits for "presence" before probing deadlocks — probing is unconditional
the moment the pacer emits. Presence (sustained box broadcast FILLER) is only a
*diagnostic* flag with a 600-frame decay, logged on gained/lost edges.

**Byte source-of-truth.** The chanmap/announce/probe blocks are the EXACT
32-byte blocks captured off the real M-5000 (reac-captures/wired-reac-a-
bothdirs-2026-06-09, master `00:40:ab:ca:15:4d`); `Sum(block[18..49]) mod 256
== 0` holds on every one. Two deliberate departures from replay-verbatim:

- **The grant is an echo, not a canned block.** The golden transcript shows the
  master echoing the box's own `cdea 04 03` back as the broadcast grant burst,
  so `REAC_M_EMIT_GRANT` stamps the received `join_blk` verbatim (~100 frames
  over the ~150 ms window — the transcribed density, not an every-slot flood).
  The §13d `0013/0e` vs `0014/0f` alternation is not byte-verifiable offline
  (the /tmp pcaps are lost); the JOIN/grant hex dumps in the event log exist so
  the first live power-cycle yields the corrective bytes if echo-verbatim is
  not enough.
- **The cfea announce embeds OUR src MAC.** The capture embeds the desk's own
  MAC in the announce payload; replaying it verbatim advertised `ca:15:4d`
  while our L2 src is `00:40:ab:00:00:01` — an inconsistent on-wire identity
  and a documented slave-disconnect trigger. `reac_master_init` rewrites the
  MAC field + recomputes the checksum.

The cdea/cfea control frames ride the 8000 fps broadcast **in-band**, occupying
audio slots exactly as the real master does (§9 reac-repacer note: control
frames replace audio slots, never add to the stream). Channel `0x13` falls in a
7th chanmap frame the 120 s capture missed (coverage 47/48 slots) — a fidelity
gap, not a link blocker.

**RX path + logging.** The pacer's TX fd (bound to the REAC NIC + 0x8819,
`PACKET_IGNORE_OUTGOING` best-effort) is drained non-blocking up to 8 frames
per slot *before* the slot's emission decision; `reac_ctrl_classify_box_frame`
(pure, offline-tested) maps each frame to BCAST_FILLER / JOIN / UNICAST / BYE
and `reac_pacer_rx_ingest` feeds the FSM on the owning thread. The SCHED_FIFO
thread never touches stdio: events go into a lock-free SPSC ring the sink
node's 200 ms main-loop timer drains to stderr — transitions with causes, JOIN
and BYE with full 32-byte hex dumps, presence edges, grant-window telemetry,
and a 10 s probing watchdog ("wire silent — check the RX path" vs "box present,
not joining — bounce the box PHY").

**Rig-day procedure** (the S-1608 hardware-verify gate; do NOT skip the PHY
bounce — the box only cold-connects on link-up):

1. `reac-pw --live <nic> --role master --tx <nic>` — the banner confirms
   event-driven mode; the log shows `IDLE -> PROBING` immediately.
2. Bounce the S-1608 PHY (replug, or link-down/up its port).
3. Read the drained transcript:
   - `box presence GAINED` proves the RX tap sees the box (its absence with a
     flooding box means the RX path is broken, not the box);
   - the `box JOIN seen (cdea 04 03 ...)` hex dump byte-confirms/corrects the
     zoneA matcher template;
   - `PROBING -> GRANTING (rx JOIN ...)` then `GRANTING -> ESTABLISHED` lines
     timestamp the full establishment (expected ~5 s cold / ~3.7 s replug per
     §13d); heartbeat lines report latency-after-chanmap (healthy: 0.5–1.9 ms).
4. On failure the shutdown summary (counters + drops by reason) says which leg
   of the courtship never happened.

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

## SLAVE path (built) — `--role slave`, slaved to an external master

In the slave role openmixer is the box: an external master (a desk, or another box
configured as master) drives the establishment and owns the clock, and we respond.
The engine is `reac_slave`, the I/O shell around the already-built slave FSM.

### S7. The slave engine (`src/reac_slave.{h,c}`)

`reac_slave` is the inverse of `reac_pacer`. Where the pacer drives `reac_master`
on its own SCHED_FIFO clock, the slave engine drives the pure JOIN/HOLD state
machine `reac_fsm` from **real RX events** and **locks to the master's cadence**:

- **One AF_PACKET socket, both directions.** Bound to the REAC NIC, it RXes the
  master's downstream and TXes our upstream on the same fd. There is **no
  `clock_nanosleep` pacer** — the master owns the rate, so each received master
  frame is one slot tick and we emit exactly one upstream frame in response
  (frame-arrival = the slot clock). A short `SO_RCVTIMEO` only self-clocks the
  presence-flood + the TX-mute dwell while no master frame is arriving yet, well
  under the 600-frame HOLD budget at every rate.
- **Establishment = the gold §13d slave sequence**, already encoded as `reac_fsm`:
  PHY-up → **flood broadcast FILLER** (presence announce) + emit the `cdea 04 03`
  cold-connect JOIN burst (sub-cmd 04, the §13b trigger) → the master cycles
  `cdea 01` sub-states (we keep announcing) → the master **GRANTS** with a
  `cdea 04 03` burst → we accept and **stop broadcasting** (TX-mute window) → the
  dwell elapses → **ESTABLISHED**: unicast our input channels upstream + a ~1/s box
  heartbeat (`cdea 01 03 0001 81`). The master MAC is **learned from the L2 source**
  of any master frame, never configured.
- **HOLD.** A master heartbeat/announce re-arms our 600-frame loop-check; if the
  master's frames stop (the clock dries up) the loop-check drains and we DROP; a
  *different* master MAC while established also drops (a slave bonds to one master).
- **TX = the upstream return.** Our box-width input channels (`box_channels`, e.g.
  16 for an S-1608) are pulled from a planar SPSC ring the PipeWire sink fills and
  placed at the box's slots by `reac_ctrl_build_upstream_filler` — a slave sends its
  inputs INTO the REAC stream, exactly as a real box does. **RX (the master's
  audio) flows through the same `reac:capture` source node as the master role**;
  only the TX direction + who-drives-the-handshake differ.

The decision core (`reac_slave_step_rx` / `_step_tick` / `_step_phy`) is a thin,
**pure** mapping from the FSM action to the concrete frame to emit — no I/O — so it
is fully offline-testable from the captured control kinds (`test_reac_slave`). The
JOIN builders it uses (`reac_ctrl_build_coldconnect` / `_config_announce`) are the
RECONSTRUCTED, experimental ones: a real link completes only when the master's
`cdea 04 03` grant is RX'd, which is the slave-side hardware-verify gate.

### Hardware-verify gate — BOTH roles (PASSED on the rig, July 2026)

Both roles were built **correct-by-construction** against the design + captures,
then taken to real hardware. The gate below is the criterion each was judged on;
the verdicts are recorded, with their captures, in the documents named.

**MASTER role** (a real Roland stagebox slaves to *us*; reac-pw is the only master
on the segment) — **PASSED 2026-07-12** on a real S-0808 and a real S-1608:

1. **The box's link state goes `establishing` → `established`** — LED SOLID,
   1/s heartbeat, zero drops. The five master-side fixes that got it there (cfea
   box-count, the missing ENROLL, the byte-exact 32-frame grant sweep, grant
   self-complete + HOLD, the explicit box heartbeat) are in
   [docs/REAC-MIXER-PROTOCOL.md](docs/REAC-MIXER-PROTOCOL.md).
2. **The box stops its presence-flood and switches to linked unicast** once our
   grant sweep lands — accepted, not merely well-formed.
3. **Audio flows** — the box's 16 mic channels reach `reac:capture` and run
   end-to-end into openmixer's console; a tone into `reac:playback` comes out the
   box's analog out (Stage B, [docs/VALIDATION-PLAN.md](docs/VALIDATION-PLAN.md),
   listen-confirmed on the braid encode).
4. **HOLD** — held across long runs with no spurious drop.

Still ungated on the master side: the **96 kHz OHRCA emit** path (protocol-sound,
never run against a real M-5000) — the procedure is in
[docs/MASTER-HARDWARE-VERIFY.md](docs/MASTER-HARDWARE-VERIFY.md).

**SLAVE role** (*we* slave to an external master) — **PASSED on a V-Mixer desk
2026-07-11, OPEN on OHRCA**:

1. **We go `establishing` → `established`** — a real, cold-booted **M-200**
   granted our cold-connect and showed reac-pw as a connected stagebox in its REAC
   menu; PROBE dropped to 0/s and the link held 300 s. The frame that unlocked it
   was the config-announce (`cdea 01 03 0010`), which we were not sending at all.
   Census + method lesson: [docs/REAC-BOX-STATE-DIAGRAM.md](docs/REAC-BOX-STATE-DIAGRAM.md).
2. **Audio flows BOTH ways** — RX proven; the upstream-return **sink** is still
   missing (W1 in [docs/SLAVE-EMULATION-SCOPE.md](docs/SLAVE-EMULATION-SCOPE.md)),
   so nothing fills `tx_ring` in the slave role yet.
3. **We lock to the master's clock** — one upstream frame per master frame, from
   `CLOCK_MONOTONIC` pacing, and a real M-200 accepts it. This **falsified** the
   earlier "clock-domain wall" conclusion.
4. **M-5000 (OHRCA) still open** — it grants us, then reverts to hunting
   (~680 probes/s). The suspect is the OHRCA established-state shape, NOT a clock
   and NOT a downstream CRC trailer (see the falsification in
   [docs/SLAVE-EMULATION-SCOPE.md](docs/SLAVE-EMULATION-SCOPE.md) W4a).

Every capture behind those verdicts is committed to `reac-captures` — **do not
leave one in `/tmp`**, which is how the original `/tmp/hs1.pcap` grant-burst
evidence was lost.

**RESOLVED (task #108, 2026-07-10):** the box's upstream return-audio layout — the
ex-"FPGA scramble" of task #61 — is the obs-h8819 even/odd channel-pair byte BRAID
over a downstream-style envelope (50 B header incl. a 16-slot `00 7a` descriptor,
`len = 52 + nch*36`, C2 EA trailer, 12 samples/frame at every rate), channel order
plain ascending. Verified against reac-captures (music channel lag1 autocorr +0.998
under the braid vs garbage under plain LE). `reac_upstream.{h,c}` decodes it; the
slave's `reac_ctrl_build_upstream_filler` now braid-packs identically, and the RX
feeder gates by role (slave rx = 1492 B downstream; master rx = box-shaped returns,
locked to the first box's src MAC).

Rig-only unknowns still open (do not guess): the 4th uncharacterised
M-5000-internal HOLD-drop trigger (REAC-CONNECTION-FSM.md gap list).

## Files

| File | Role |
|---|---|
| `src/main.c` | CLI + lifecycle: parse `--role`, open feeder, create source node, then (master) the sink or (slave) the slave engine; run the loop |
| `src/reac_role.h` | **role selection**: `--role master\|slave` parse + validation (slave requires `--tx`), header-only + unit-tested |
| `src/reac_ring.{h,c}` | lock-free SPSC planar-float ring (RX hot-path → process(); also the slave's upstream-input carrier) |
| `src/reac_rx.{h,c}` | non-RT feeder: wire source (live/pcap) → libreac validate → role-gated decode (downstream 40-ch / upstream box return) → f32 → ring; counter-slope ppm estimator |
| `src/reac_source_node.{h,c}` | `reac:capture` pw_filter: 40 F32 ports, RT process(), follower/driver clock (RX for BOTH roles) |
| `src/reac_tx.{h,c}` | downstream-frame encoder (`reac_tx_build`, inverse of the decode core) + raw-socket emitter |
| `src/reac_upstream.{h,c}` | UPSTREAM (box return) audio decode: box-width braided frame → planar s24 (task #108) |
| `src/reac_master.{h,c}` | **master-role** JOIN/HOLD: the cdea/cfea establishment FSM + captured control-block templates (S2) |
| `src/reac_pacer.{h,c}` | **master-role** SCHED_FIFO cadence pacer + TX frame ring; stamps the master block on egress (S6) |
| `src/reac_sink_node.{h,c}` | `reac:playback` Audio/Sink: process() encodes + submits to the pacer (the master TX) |
| `src/reac_ctrl.{h,c}`, `src/reac_fsm.{h,c}` | the slave control plane: virtual-stagebox builders/parser/checksum + the pure JOIN/HOLD FSM |
| `src/reac_slave.{h,c}` | **slave-role** engine: drives `reac_fsm` from RX events, locks to the master cadence, returns our inputs upstream (S7) |
| `tests/test_reac_ring.c` | SPSC ring unit test (round-trip, underrun, overrun) |
| `tests/test_reac_tx.c` | TX encoder ↔ decode-core round-trip (24-bit ULP, no channel cross-wire) |
| `tests/test_reac_master.c` | master cdea/cfea blocks byte-match the captures + checksum + chanmap walk + FSM sequence |
| `tests/test_reac_pacer.c` | pacer slot period (125/250/272 µs) + SPSC frame ring + live ~8000 fps emit (needs CAP_NET_RAW) |
| `tests/test_reac_slave.c` | slave establishment §13d (flood→probe→grant→mute→established upstream + heartbeat) + HOLD, from the captured master control kinds |
| `tests/test_reac_upstream.c` | upstream decode vs REAL sanitized captured frames (628 B/16 ch + 340 B/8 ch, full PCM tables) |
| `tests/test_reac_rx_gate.c` | RX stream gate: role picks downstream vs upstream; first-box src-MAC lock; ring contents end-to-end |
| `tests/test_reac_role.c` | `--role` parse + validation contract (master default; slave requires `--tx`) |
| `meson.build`, `meson_options.txt` | build: pipewire/spa via pkg-config, libreac subproject, reac-aes67 core sibling |
| `subprojects/libreac.wrap` + `packagefiles/libreac/meson.build` | libreac as a meson subproject |
