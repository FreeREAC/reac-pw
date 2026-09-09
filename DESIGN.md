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
original behaviour). Both roles share the same encoder/decoder (libreac's
`reac_downstream_build` / `reac_braid_encode` and its decode core) and the same PipeWire nodes (`reac:capture` for RX,
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

Reuses, does not reinvent — **libreac >= 0.5.0** (`FreeREAC/libreac`,
`<reac/*.h>`) is the single REAC byte-layout oracle and the only REAC dependency:
frame validate, the byte-14/15 counter, gap math, `reac_detect_rate_fd` /
`reac_rate_snap`, the braid codec (`<reac/reac_braid.h>`), the f32↔s24 sample pair,
the box-upstream decode (`<reac/reac_upstream.h>`), the OHRCA +2 length rule
(`reac_frame_clean_len`), plus `reac_decode.c` (the downstream decode — braided
since 0.5.0, the old plain-LE layout surviving only as the named diagnostic
`reac_decode_plain_le()`, #80), `reac_capture.c` (AF_PACKET 0x8819) and
`pcap_source.c` (classic pcap reader). The floor is 0.5.0 because a 0.4.x
libreac links fine and then reads the downstream with a layout our own encoder
does not write. There is **no reac-aes67 sibling checkout any more** — the three
compiled-straight-in files and reac-pw's own copies of the braid, the s24
conversion and the upstream decode were all folded into libreac 0.3.0 on
2026-07-28 (`be52b83`, `87297ca`, `e6f1ca7`, `2abeed4`). Never fork a second
decoder.

reac-pw itself is the lock-free ring, the RX feeder, the control plane
(cdea/cfea/DT1 builders + the two checksums), the establishment FSMs, the pacer
and clock discipline, the head-amp send model, the box registry and the PipeWire
nodes — see the Files table at the end.

## Data path (RX, Phase 1)

```
REAC NIC (AF_PACKET 0x8819)  ─┐
                              ├─► reac_rx feeder thread (SCHED_OTHER, producer)
pcap replay (offline test)  ─┘     reac_frame_is_reac / reac_frame_counter   [libreac]
                                   reac_decode → planar s24 → f32             [libreac]
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
pthread, NOT SCHED_FIFO — it's the producer) reads frames, decodes with
libreac into planar f32, and `reac_ring_write`s whole REAC frames (40 ×
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
each 12-sample group with libreac's `reac_downstream_build` and submits it to the SCHED_FIFO
pacer, which clocks the wire at a fixed pps and drives the master JOIN/HOLD
handshake so a real Roland stagebox slaves to us. See the TX-path section (S2/S6)
below. (Despite the name it presents as the *master*, not a stagebox — the
downstream broadcast is the master's role; a true virtual-stagebox upstream is
the separate `reac_ctrl`/`reac_fsm` slave half.)

## Build (meson + pkg-config)

`meson.build` + `meson_options.txt`. External deps via pkg-config: only
`libpipewire-0.3` and `libspa-0.2` (Fedora `pipewire-devel`) plus `threads` and
optional `m`, plus libreac. No libpcap (the pcap reader is libreac's
dependency-free `pcap_source.c`; live capture is raw AF_PACKET).

**libreac >= 0.3.0** resolves to the system `libreac-devel` (pkg-config) when new
enough, else the **meson subproject** fallback (`subprojects/libreac.wrap`, a
`wrap-git` on `FreeREAC/libreac`) takes over. Upstream libreac ships only a hand
Makefile, so the wrap drops a small `meson.build` into the checkout via
`patch_directory` (`subprojects/packagefiles/libreac/meson.build`) that builds it
into a static lib and exposes `libreac_dep`. No upstream source is touched.
`meson_options.txt` carries **no options** — the old `-Dreac_aes67=<path>` sibling
checkout is gone (see "Reuses, does not reinvent" above).

```
meson setup   build
meson compile -C build
meson test    -C build            # 29 tests, no PipeWire and no hardware needed
./build/reac-pw --pcap capture.pcap --rate 48000
sudo ./build/reac-pw --live reac0           # needs CAP_NET_RAW
```

Everything but `test_reac_pacer`'s live-cadence case runs anywhere; that one SKIPs
(exit 77) without `CAP_NET_RAW`. The node code needs `pipewire-devel` installed to
compile.

## TX path (built) — `reac:playback` is a working REAC MASTER

`reac:playback` now emits real REAC downstream and presents as the **master**, so
a Roland stagebox slaves to it. The path is: graph → `reac_downstream_build` (encode, libreac) →
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
`[16:18]` + control block `[18:50]` over the frame `reac_downstream_build` produced,
re-applying the cdea/cfea checksum and leaving audio + counter + `C2 EA` tail
intact).

**EVENT-DRIVEN establishment (task #130).** A real desk never advances the
handshake on a timer — the earlier cut auto-advanced PROBING→GRANTING after ~1 s
and reached ESTABLISHED against a silent wire. Every FORWARD transition out of
PROBING is gated on a received, validated box control frame.

**The state diagram lives in one place: [docs/MASTER-FSM.md](docs/MASTER-FSM.md).**
It is read from `src/reac_master.c` + `src/reac_master_fsm.c`, states, entry
actions, triggers and deliberate ignores, each with the rig date that produced it.
The copy that used to sit here drifted within two days of being written — it still
showed GRANTING as an ECHO of the box's JOIN block, and a grant-window-expiry
timer back to PROBING. Both were replaced on the rig 2026-07-12: the grant is a
GENERATED enrollment sweep (below), and the window expiry became a forward
self-complete because timing back to PROBING made a real S-0808 re-attempt forever
(27 grant-timeouts, LED blinking faster). `DROP_GRANT_TIMEOUT` is now vestigial.

Two properties this file is still the right home for, because they are design
rules rather than transitions:

- The counter free-runs across every transition.
- There is no presence gate. The box only emits its cold-connect on a real PHY
  link-down/up (§13b), so a master that waits for "presence" before probing
  deadlocks against a box whose PHY never bounced — probing is unconditional the
  moment the pacer emits. Presence (sustained box broadcast FILLER) is a
  *diagnostic* flag with a 600-frame decay, logged on gained/lost edges.

**THE BOX COMES FROM THE WIRE (2026-08-05).** The master starts knowing nothing
about any box: no allocation, no enrollment sweep, and no way for anything typed to
supply one. `reac_master_set_box` — driven by the box's own config-announce, matched
against the fixed model matrix — is the only door in, and `reac_master_forget_box`
(run on every backward transition to PROBING) the only way out, so a swapped box can
never inherit a departed one's head-amp base. No box present is a normal running
state, not an error; a cold-connect JOIN carries no width, so GRANTING with nothing
declared HOLDS the ungranted announce a real M-200 holds and then falls back to
probing rather than guess. `--box` is retired (accepted, ignored, reported once); a
SLAVE's `--box-channels` stays, because our own width is a fact about us with no wire
to learn it from.

What this removed: the grant used to be allocated at init from `cfg.in_channels`,
whose only ever value was the S-1608's 16 (hard-coded in `reac_sink_node.c`). A box
that reached GRANTING before declaring its model was granted THAT enrollment — head-amp
slots `0x20..0x2f` claimed for a box whose inputs may live at `0x00..0x07`. Recognition
corrected it a moment later for every box in the matrix, which is why it stayed latent;
a box outside the matrix, or one whose config-announce was lost, had nothing to correct
it, and the failure is silent by construction (the box links, streams audio, and ignores
every head-amp record — the class `reac_grant.h` records from 2026-07-17).

The master publishes what it decided so a consumer never has to re-derive it:
`reac.box-model` / `reac.box-width` / `reac.headamp.channels` as before, plus
`reac.box-source` (`wire` while a box is known, `none` otherwise),
`reac.headamp.base` — the head-amp wire channel the box's input 1 sits at, i.e. the
`base` in `CH = base + (input - 1)` — and `reac.box.mac`, the ENROLLED BOX's own L2
address as latched from its JOIN, colon-separated lowercase, `none` when no box is
known. That last one is the peer's address and never ours: `reac.master.mac` names
whoever drives the segment, which in the master role is this NIC, so a consumer
keying a box registry on it matches nothing on every rig. `reac_link_state.h`
defines all of them.

**Byte source-of-truth.** The probe/SUB01/SUB02 blocks are FIXED protocol
constants replayed verbatim from a real **M-300** driving an S-1608
(`reac-captures/m300-s1608-*.pcap`, 2026-07-10, master `00:40:ab:c9:d8:5b`);
`Sum(block[18..49]) mod 256 == 0` holds on every one. Chanmap and cfea are
GENERATED and diffed against those captures. The per-console identity byte is the
only thing that varies across M-200 / M-300 / M-5000 —
`tests/test_reac_conformance.c` proves the three differ on the wire in exactly
two bytes (source MAC + console field) and share one generator for the rest.

Two deliberate departures from replay-verbatim:

- **The grant is a GENERATED enrollment sweep, not an echo and not a canned
  block.** This originally said the master echoes the box's own `cdea 04 03`
  back, and `REAC_M_EMIT_GRANT` stamped the received `join_blk` verbatim. That
  is wrong and it cost a rig session: a burst transcribed from an M-200 granting
  an S-0808 (8 inputs at base `0x00`), replayed at a 16-input S-1608 (base
  `0x20`), LINKS and streams audio and then ignores every head-amp record —
  28 byte-perfect records in 40 s, 48 V never lit, because our grant never
  claimed those slots. `src/reac_grant.c` now generates the sweep over the slots
  WE allocate to the connected box, and `REAC_M_EMIT_GRANT` stamps one block of
  it. The load-bearing property is an agreement: the slots the sweep enrols must
  be the slots our head-amp traffic later addresses — both halves pinned by
  `tests/test_reac_grant.c`.
- **The cfea announce embeds OUR src MAC.** A captured announce carries the
  desk's own MAC in its payload; replaying that verbatim advertises one identity
  while our L2 source is another — an inconsistent on-wire identity and a
  documented slave-disconnect trigger. `gen_cfea` always writes OUR MAC and
  recomputes the checksum.

The cdea/cfea control frames ride the broadcast **in-band** (fps = rate/12, so
4000 at 48 kHz and 8000 at 96 kHz), occupying audio slots exactly as the real
master does (§9 reac-repacer note: control frames replace audio slots, never add
to the stream).

The chanmap coverage gap this paragraph used to record — "channel `0x13` falls in
a 7th chanmap frame the 120 s capture missed, coverage 47/48" — is closed. The
sweep is generated, not replayed: `gen_chanmap` emits the full
`REAC_M_CHANMAP_RING` = 49 windows (the 48 head-amp positions `0x00..0x2f` plus
the `0xfe` section marker), measured live off an M-200 driving an S-1608 (#130).
That was the fix for the class where a one-frame `0x00..0x06` map left every box
mute: a box enrols only after it sees the window mapping ITS OWN slots, so the map
has to be space-wide, not console-width.

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
`SCHED_FIFO` in the wire-clock band below the PipeWire graph (`reac_rt.h`,
`REACPW_RT_PRIO`), CPU-pinnable, woken every slot period by
`clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` on an absolute deadline that
advances by `period_ns` each tick (125.0/250.0/272.1 µs; no drift accumulation,
snap-forward on a late wake so we never burst-catch-up).

`on_process()` (the RT graph callback) only **encodes + submits**: it builds whole
1492-B frames with `reac_downstream_build` and pushes them into a lock-free SPSC frame
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
   (~680 probes/s). The suspect is the OHRCA established-state shape, and not the
   clock. Whether a downstream trailer is in play depends on the unresolved
   reading of the downstream `+2` — see
   [docs/SLAVE-EMULATION-SCOPE.md](docs/SLAVE-EMULATION-SCOPE.md) W4(a) and #80.

Every capture behind those verdicts is committed to `reac-captures` — **do not
leave one in `/tmp`**, which is how the original `/tmp/hs1.pcap` grant-burst
evidence was lost.

**RESOLVED (task #108, 2026-07-10):** the box's upstream return-audio layout — the
ex-"FPGA scramble" of task #61 — is the obs-h8819 even/odd channel-pair byte BRAID
over a downstream-style envelope (50 B header incl. a 16-slot `00 7a` descriptor,
`len = 52 + nch*36`, C2 EA trailer, 12 samples/frame at every rate), channel order
plain ascending. Verified against reac-captures (music channel lag1 autocorr +0.998
under the braid vs garbage under plain LE). libreac's `<reac/reac_upstream.h>` decodes it (the local copy was deleted in
`87297ca`); the
slave's `reac_ctrl_build_upstream_filler` now braid-packs identically, and the RX
feeder gates by role (slave rx = 1492 B downstream; master rx = box-shaped returns,
locked to the first box's src MAC).

Rig-only unknowns still open (do not guess): the 4th uncharacterised
M-5000-internal HOLD-drop trigger (REAC-CONNECTION-FSM.md gap list).

## 0.5.0 — what the daemon decides for itself, and what it still owes

Nobody writes a config file. Started with no flags and no conf, reac-pw finds its own
segments and takes its own role on each of them, and the three things it does NOT yet do
are named here rather than left to be rediscovered.

**What it does.** Every linked, non-loopback Ethernet interface gets a passive `0x8819`
sniffer, which transmits nothing; the first frame that classifies as REAC turns that
interface into a SEGMENT (`reac_ifscan`). The role then comes out of the same hearing
(`reac_hunt`): a desk mastering the wire is joined as a slave; a wire with a box on it
and no master is taken as master after three master announce cadences (3 s) and the box
is granted; a stagebox strapped to master is JOINED as a slave at the width it announces
(0.5.1's ruling below) and is refused only where the operator pinned that wire master;
a `REAC_ROLE_<segment>` pin skips the election and the hearing both, and is
served **on link up**; and an UNPINNED linked wired interface that carried nothing at all
for 500 ms has been PROVEN masterless and is taken as master through the ordinary master
role (see "A cold stagebox is silent" below). **The segment's NAME is
its interface's**, and so is the node suffix
(`reac-capture.enp131s0`), which is also the key the per-segment conf is written under —
`REAC_ROLE_enp131s0`. There is no per-interface file any more; there is one conf and
suffixed keys in it.

**A cold stagebox is silent, so hearing alone cannot wake one.** Measured on the
operator's desk 2026-09-08 22:10 with 0.5.0-2: an S-0808 on `enp131s0` and an S-1608 on
`enp128s20f0u2`, both freshly powered, both cabled, both NICs carrier up at 100 Mb full,
and in five seconds `rx_packets` moved by **0** on both and eight seconds of `tcpdump`
caught no `0x8819` frame at all. A REAC stagebox in SLAVE mode transmits nothing until a
master announces to it — the `cdea 04 03` JOIN and the heartbeats this rig has captured
all followed a master's announce, never preceded one. So "the first classifying frame is
the gate to SERVE" is a gate a cold segment can never open, and 0.4.8 did not have this
problem only because it drove from the first instant with no gate at all. This is the
hearing model's one hole and it is not a small one: two boxes, both mute, forever.

Therefore the model needs an answer that does not depend on being spoken to first, and
there are two. Both are built, because a final-user system has no pins on its first boot
and the pin alone would leave that system exactly as mute as the rig was.

**The pin, for a wire the operator answered for.** `REAC_ROLE_<iface>=master` is an
explicit answer about that wire, and a setting is not evidence to be weighed — so a
PINNED interface opens its side the moment it has CARRIER, with no frame required: a
pinned master probes and announces exactly as 0.4.8 did, a pinned slave opens its slave
engine. *(Proven on the rig with 0.5.0-3: both boxes came up within two seconds of
`pinned master — driving on link`.)* A pinned SLAVE is not silent, and the prose here used
to claim it was: on PHY-up the slave role FLOODS `REAC_FSM_FLOOD_BURST` (~5460) broadcast
FILLER frames and only then falls quiet if nothing answered. That is the protocol's own
bounded cold-connect announcement and is exactly what a real box does, so it is right —
it is simply not silence, and the journal says `pinned slave — cold-connect flood, then
listening for a master`.

**Driving on proven silence, for a wire nobody answered for — the DEFAULT on every
unpinned linked wired interface.** Such an interface is listened to for
`REAC_KNOCK_LISTEN_NS` (500 ms), and if it carried NOT ONE frame it is taken as MASTER
through the ordinary master role: the same pacer, the same probing-until-the-box's-
cold-connect, the same NIC address as a pin. There is no second emitting path and no
special frame.

**One announce is not enough, and that was measured.** The first cut of this sent a single
master announce every two seconds and waited for a reply. On the rig, 2026-09-08, with
0.5.0-3 installed and `enp128s20f0u2` unpinned: the daemon logged `no REAC heard —
knocking (announce every 2 s)`, tx rose by two frames per six seconds, and **rx stayed at
+0 for over a minute**. A cold box does not answer a lone announce; it answers a master
that is DRIVING — the continuous stream the pinned path and 0.4.8 both send, and the one
that brought both boxes up in two seconds. The lone-announce knock is gone, along with the
Roland-OUI stand-in source address it emitted from (`reac_mac.h`'s own law forbids one:
two reac-pw hosts on a segment each dismissed the other's frames as its own echo, and a
box that learned the stand-in met the served master's real address and dropped on
`FSM_DROP_MAC_CHANGE`).

**Why driving is safe, which is the whole argument for transmitting at all (operator
ruling 2026-09-08, "no traffic, no master").** A REAC master transmits CONTINUOUSLY at the
wire cadence: one frame per audio slot, ~125 µs at 96 k, ~272 µs at the slowest rate this
daemon serves. *A master cannot be present and silent.* So 500 ms of nothing — 1837
consecutive slots at that slowest cadence, and more than two of the 200 ms hearing polls —
is PROOF the port is masterless, not a guess, and only a port proven masterless is ever
driven. Any frame inside the window cancels the licence outright and the ordinary hunt
rules: a desk is joined, a stagebox strapped to master is joined too (0.5.1), and a box
with no master on its wire is granted.

**And a bet stays watched.** A wire taken on silence KEEPS its sniffer, which every other
segment drops when it is served, because this one was served on a bet that nothing was
there — and the thing it bet against can only ever turn up later. If a foreign master that
is a DESK is heard on it, the segment is handed over at once: master down, slave up, no
shouting (`hearing_yield`, the arbitration's observe-then-act law) — and since 0.5.1 a
STAGEBOX that starts mastering it is yielded to on the same terms, because that wire is
unpinned and a box that wants the clock gets it. Only a wire pinned master refuses, and it
keeps its own master rather than yielding: dropping it would take the segment from every
other box on it to nobody's benefit. This closes, for wires taken this way only, the
re-resolution item that the list below still owes in general.

**The accepted cost, ruled by the operator 2026-09-08.** A linked wired interface with
nothing on it is driven at the master cadence indefinitely, and on an office LAN that is a
REAC stream nothing will ever answer. 0.4.8 did exactly this on every interface it was
given. **Wireless is excluded twice over**: it is out of the interface scan by default, and
even where the operator allowlists one in with `REAC_IFACES_ALLOW_WIRELESS` that allowlist
buys LISTENING only — an associated Wi-Fi interface is quiet of 0x8819 by nature, so
silence there proves nothing about a REAC master, and it is never driven on silence. It is
still served the moment REAC is actually heard on it, which is evidence and not a bet.

**The journal says which of the three an interface did**, in one line at link up:
`pinned master — driving on link`, `pinned slave — cold-connect flood, then listening for
a master`, or `unpinned — listening for REAC`; and, when a silent wire is taken,
`no REAC heard in 500 ms — a master fills every slot, so this wire has none: taking it as
MASTER and probing until a box cold-connects`.

**What it owes, in this order.**

1. **Node names that follow the BOX, not the segment.** A box does not belong to a
   segment — its patch should survive being moved to another port — so the instance
   suffix should derive from the box's identity, not from the interface. `reac_ifname`
   (bus + physical address) is built and tested for the groundwork and is deliberately
   wired to nothing: swapping segment identity today would rename every per-segment key
   and every console patch in one step, which is a migration, not a refactor.
2. **Trunk topology — DONE in 0.5.3**, below. Hearing 802.1Q-tagged frames on a
   physical parent, and adopting or creating the sub-interfaces that carry them, is
   built and proven on veth; what is still owed is the rig itself, which has never been
   on a trunk port. A segment is still a whole interface — a VLAN sub-interface is one.
3. **Re-resolution after a segment is up — DONE in 0.5.4**, below. Every wire WE took
   and nobody pinned keeps its sniffer now, whether it was won on proven silence or on
   a box heard, and a desk that turns up second is yielded to on either. A pinned
   segment still keeps its role: a pin is the operator's answer about that wire, and the
   only thing a rival can do to one is the 0.5.1 refusal.

**Versioning.** 0.5.0 is this release. The increments above go 0.5.1, 0.5.2, ... — the
middle digit does not move again for them.

## 0.5.1 — a box that masters the wire is JOINED, unless we forced master

**Operator ruling, 2026-09-09, after the rig proof.** An S-0808 was rebooted with its REAC
Mode switch on M and left on an unpinned wire. 0.5.0-3 logged `REFUSED (rival-master-box):
… masters this wire at 8 ch, which is a BOX width, not a desk's 40 … Nothing is transmitted
here and nothing is fought`, served nothing, and the segment vanished from the console —
no node, no props, no remedy, just an absence. The ruling: *if the box wants to be master,
unless we have forced the master mode, we can enslave the segment to the box's master
clock.* The sentence this file used to carry — never slave-join a box — was not merely too
narrow, it was WRONG. Taking a clock from the wire is what a REAC endpoint does, and which
end of the pairing sent it changes nothing about the clock.

So the wire decides, and the only refusal left is a contradiction the operator wrote down:

| the wire | `REAC_ROLE_<segment>` | what the daemon does |
|---|---|---|
| a DESK masters it | unpinned, or `slave` | joined as a slave — unchanged |
| a BOX masters it | unpinned, or `slave` | **joined as a slave**: its clock, its width |
| a BOX masters it | `master` | REFUSED (`rival-master-box`), and the segment still publishes a DOOR |
| an unreadable rival | unpinned, or `slave` | REFUSED (`rival-master-unknown`) where the wire was ours to take: a frame kind nobody has captured must not be JOINED either |
| an unreadable rival | `master` | the pin stands and drives — §4's conservatism cuts both ways, and a frame nobody has captured must not flip a pinned segment's topology |

Only a BOX is read sharply enough to refuse a pin: its geometry is unambiguous, and the
remedy is a switch on its front. A pin is the operator's own answer about that one wire, so a
pinned master beside a box on M is the single case where two answers contradict each other — and the daemon never settles
that by out-shouting. It says so, and the remedy is the box's own switch. Everywhere else
the wire is obeyed, in the journal's own words: `box masters this wire — joining it as a
slave (operator rule: a box that wants to be master gets the clock)`.

**WHERE THE PINNED REFUSAL IS TAKEN, AND WHY IT IS NOT IN THE HUNT.** A pin is served ON
LINK with no frame waited for — that is the 2026-09-08 rule and it is not weakened here: a
cold stagebox in slave mode transmits nothing until a master announces to it, so a pinned
wire that waited for evidence would wait forever. The sniffer's socket is opened in the same
200 ms poll that takes the hunt's decision, so at that instant its table is empty BY
CONSTRUCTION; and a box on M announces its master signature about once a second, so even a
listening window would cost an announce cadence of added latency on every pinned wire,
silent or not. Measured on the veth proof: the hunt-side refusal fired zero times out of
every run. So the pin drives, and the segment's OWN engine — which already classifies every
frame on that wire into the discovery table the console reads — is what notices. About a
second later the segment comes down and the door goes up in its place, and the journal says
`we drove it until we heard it and we stop now`. The hunt keeps the same rule for the case
it CAN see (a rival already in its table when the wire is served), so the decision table
above is the whole law and only its timing depends on which half of the daemon reached it
first.

*(This is also where a real defect surfaced: a sighting's WIDTH never crossed the pacer's
event ring — it carried the role and the model index and nothing else — so the master side
classified every box on M as an unreadable rival and published `rival-master-unknown` for
it. The geometry is what §2b decides on, so it now rides the ring with the rest.)*

**AND THE REFUSAL IS NOT A LATCH.** A refused segment keeps a sniffer, exactly as a wire
taken on proven silence does, and the door comes down when the rival stops mastering the
wire — bounded by the discovery table's own 5 s withdrawal window, because an EMPTY table is
not evidence that anybody left. Without that bar the door came down 200 ms after going up
and went back a second later, which is what a flap looks like on a console.

**What a box-master wire carries, and how it differs from a desk's.** The mode switch is read
at boot and never re-read, and M is the SPLITTER's clock role rather than "act as a console":
the S-4000S image carries a master parser AND a slave parser plus a clock driver, so it is
clock-slave on its uplink and master on its split outputs
(`reac-protocol/wire-format.md`, "The stagebox's REAC Mode switch — M / S / SP"). Three
consequences decide everything below.

- **The geometry is the box's own, not the fabric's.** A box on M broadcasts its UPSTREAM
  geometry and never a master downstream frame: `52 + n × 36` bytes — 340 B at 8 channels,
  628 B at 16, 1204 B at 32, the same at every rate — where a desk's downstream is the fixed
  1492 B, 40-channel broadcast (`wire-format.md`, "Upstream (stagebox→master) audio layout").
  That width IS the classification (`reac_rival_kind_from_channels`), and it is also what the
  segment's nodes are sized to: 8 ch of box means an 8-port `reac-capture`, never a 40-slot
  fabric with 32 silent rows.
- **The packing is the same braid in both directions**, confirmed on real captures at all
  three widths, so a box-master stream is decoded by the oracle the master role already uses
  for a box's return — `REAC_RX_ACCEPT_UPSTREAM` and `reac_upstream_decode`. No second
  decoder, and no new frame kind.
- **A box on M runs no handshake at all.** Measured: zero control frames — no announce, no
  grant, no heartbeat — so nothing pairs with it in either direction; it cannot be granted
  (it never cold-connects) and it cannot be enrolled with (it never grants). Both were tried
  (`wire-format.md`). With no uplink it free-runs at its last-known rate, measured +363 ppm
  off nominal against −16 ppm for an enrolled box on the same rig.

**Therefore the join is RECEIVE-ONLY, and that is the honest shape rather than a reduced
one.** The segment locks to the arrival cadence, decodes the box-width stream into a
`reac-capture` sized from what the box announces, and publishes the aggregate. It does NOT
open the slave engine: that engine exists to answer a grant, and a peer that emits no control
frame will never send one — a cold-connect flood aimed at it would be noise with a state
machine behind it. What the operator gets is the box's channels in the graph, and props that
name whose clock they arrived on.

*(The rig's own S-0808 on M, 2026-09-09, put a MASTER-role frame on the wire beside that
box-width geometry: the 0.5.0-3 refusal cannot fire without one, since only
`REAC_DISCO_ROLE_MASTER` evidence reaches `reac_arbitrate`'s `foreign_master`. The S-4000S
"zero control frames" measurement above is that box on that day; the two are not in conflict
about anything this code depends on — the width is what classifies, and the width agreed.)*

**A REFUSED WIRE STILL PUBLISHES A DOOR.** The 2026-09-09 rig proof's real cost was not the
refusal, it was the SILENCE: a wire the daemon had decided about, and a console with nothing
to render. A refusal that cannot be seen is indistinguishable from a daemon that is not
running. So a refused segment is served as a door-only segment:

- **One node**, `reac-capture.<segment>`, no ports carrying audio and no engine of any kind
  behind it — no TX, no pacer, no segment lock, no RX feeder. Nothing is transmitted, and
  now nothing is received either: the refusal is total, and the door is a statement about it.
- **Not `reac-playback`**, although a segment pinned master would ordinarily carry its door
  there: that node exists only where a pacer drives the wire, and publishing one over no
  pacer would be a door onto an engine that is not there. One segment, one door, and the
  door is on the node that exists.
- **The props are the refusal, in the vocabulary that already exists** (`reac_link_state.h`,
  `reac_segment_ident.h`) — never a second spelling of it:

  | property | value |
  |---|---|
  | `reac.segment` | the segment's name, so a console can key its row on it |
  | `reac.master.state` | `foreign` |
  | `reac.master.mac` | the RIVAL's MAC — who is mastering this wire |
  | `reac.master.rival.kind` | `box` (or `unknown`) |
  | `reac.master.refusal` | `rival-master-box` (or `rival-master-unknown`) |
  | `reac.pace.source` | `foreign-master` |
  | `reac.master.conflict` | `0` — we are not mastering, so the mid-flight dispute cannot exist |

  The remedy is not a property: `refusal` + `rival.kind` + the MAC are what a surface renders
  one from, and the journal already carries the sentence.

**And the yield follows the same table.** A wire taken on proven silence keeps its sniffer,
and a box that turns up on it and masters it is now yielded to exactly as a desk is — master
down, receive-only slave up — because the verdict, not the rival's kind, is what
`hearing_yield` acts on.

## 0.5.2 — a joined box master is a BOX, and it says which one (2026-09-09)

**Measured on the rig with 0.5.1, 05:45.** The S-0808 on M was joined exactly as 0.5.1
ruled: `reac-capture.enp128s20f0u2` came up at 8 channels on the box's own clock, carrying
`reac.segment`, `reac.master.state=foreign`, `reac.master.rival.kind=box`,
`reac.master.mac=00:40:ab:c4:dc:9c` and `reac.rate=96000`. The console rendered a segment
and NO STAGEBOX: it keys a box off `reac.box.mac` / `reac.box-model` / `reac.box-width` /
`reac.link-state`, and the join published none of the four — so the same chassis that is
`box-676b3a9a` when we master it was a nameless width when it masters us, its inputs
unpatchable. The node also read `reac.cfg.role.state=role_reestablish_pending` while the
segment was up and streaming.

**Same box, same MAC, same patches — whichever end of the pairing sends the clock.** A
box's identity is the box's own; which of the two ends is mastering is a fact about the
WIRE and belongs in the `reac.master.*` aggregate, where it already is. So a joined box
master publishes the identity set a served box publishes, on the segment's one door (the
capture node — a receive-only join has no playback node, `reac_segment_ident.h`).

**WHERE THE IDENTITY COMES FROM: THE WIDTH, BECAUSE THERE IS NO ANNOUNCE TO READ.** A
served box names itself in a cold-connect config-announce (link 1, opcode 0x82/0x84), and
`reac_ctrl_identify_box` matches that 32-byte descriptor block against the fixed matrix —
which is how the disco printed `recognized box = S-0808` for `00:40:ab:c4:dc:9c` on the day
we mastered it. **A box on M sends no such frame.** What it broadcasts is its UPSTREAM
geometry, `52 + n × 36` bytes (340 B at 8 channels), and the one control-bearing frame in
it carries a master-only record, not a declaration: `reac_ctrl_identify_box` returns NULL on
every frame of that stream, which is why the rig's own `REAC heard — master
00:40:ab:c4:dc:9c (8 ch)` line named no model where a served box's names one.

So the model is IMPLIED BY THE WIDTH, and only where the implication is exact:

- `reac_box_master_model(width)` returns the matrix row whose `in_ch` EQUALS the broadcast
  width, and NULL otherwise. It is deliberately not `reac_box_model_by_channels`, which
  falls back to the S-1608 row for an unmatched width (`libreac reac_ctrlblk.c:521`) — the
  same trap `reac_disco.c`'s classifier already refuses: a default would name a box that was
  never identified.
- `reac.box-model` and `reac.box-width` are published from that row (`s0808`, `8x8`), so the
  console folds the join into the box it already knows by MAC and the operator's patches
  survive the mode switch. A width no row matches publishes NEITHER key — absence is a fact,
  and the width is still on the graph as the node's ports and its description.
- `reac.box.mac` is the mastering peer's own address, from the sighting that decided the
  verdict. A box on M grants nothing, so the sighting is the only evidence there is — the
  same address `reac.master.mac` already carries, and deliberately both: one says WHO IS
  MASTERING THIS WIRE, the other says WHICH CHASSIS THESE INPUTS ARE.
- `reac.link-state` is `probing` until the stream is locked and `established` once it is,
  where locked means the segment's own RX is accepting the box-width frames it decodes into
  the graph (`reac_segment_heard_step`, the same evidence `reac.master.state=foreign` rests
  on). There is no `granting`: nothing is granted in either direction here.

**AND THE ROLE IS APPLIED, NOT PENDING.** `role_reestablish_pending` means the engine asked
for is not the one performing; a receive-only join was reading it because the answer was
derived from `reac_slave`'s enrolment flag and no slave engine runs here. It never will:
that engine exists to answer a grant. The receive-only join IS the slave role performed —
the segment follows the box's clock and delivers its channels — so it derives its answer
from its own engine instead (`reac_role_engine_of_receive_only`): `role_hunting` while the
wire has not been heard yet, `applied` once it is.

**WHAT A BOX-MASTER WIRE CARRIES — the receive-only contract, stated as the operator reads
it.** Its inputs arrive: the box's mic channels, at the box's width, on the box's clock.
Nothing returns to it — no audio, no head-amp, no control frame of any kind — because it
runs no handshake to receive one. And its OUTPUTS ARE NOT OURS: an S-0808 on M is splitting
its 8 outputs from a stream we are not driving, so this segment has no `reac-playback` node,
publishes no head-amp keys, and an operator cannot route to that box from here. The mode
switch on the box's front panel is what changes any of that.

## 0.5.3 — the TRUNK: a VLAN is a segment, and the daemon makes the netdev (2026-09-09)

0.5.0's owed list called trunk topology designed and not implemented: *nothing in `src/`
reads a VLAN tag*. This is that increment. The reference topology and every ruling behind it
are openmixer's `docs/design/specs/2026-08-23-reac-trunk-vlan-daemon.md` (§3 the kernel
measurements, §4 the detector and the create path, §5 a segment IS an interface, §12 the
admin half); what follows is the contract as reac-pw implements it.

**THE DESIGN IS A SUBTRACTION AND STAYS ONE.** Nothing in the audio path changed and no tag
is parsed in it. A trunk is served by learning WHICH VLANs carry REAC and handing the kernel
one `<parent>.<vid>` netdev per VLAN; from there each is an ordinary interface and every
mechanism this daemon already has — the sniffer, the hunt, the pin, the silence licence, the
seglock, the per-segment conf keys, the nodes — runs on it unchanged, because §5 rules that a
segment IS an interface and a VLAN sub-interface is one.

**WHAT THE KERNEL GIVES, MEASURED ON THIS KERNEL.** 7.2.4-200.fc44, 2026-09-09, openmixer's
`tools/probe-vlan-8819.py` under `unshare -rn` (a veth pair in a private netns, no NIC), the
same five scenarios the spec measured on 6.x, and the same five answers:

| | observed |
|---|---|
| A | a socket on `vtrunkB.111` bound to `0x8819` receives a frame that arrived tagged on the parent, **untagged, with no 802.1Q header in the buffer** |
| B | a socket ON THE PARENT bound to `0x8819` receives **the same frame as well**, `vlan_tci = none` — indistinguishable from an untagged frame |
| C | an `ETH_P_ALL` tap on the parent sees it with `PACKET_AUXDATA vlan_tci = 111`. **The tag is kernel metadata, never bytes** |
| D | TX on the sub-interface egresses tagged; the tag is inserted by the kernel and is absent from the buffer we wrote |
| E | a frame on a VID with **no sub-interface** still reaches the parent, and only the `ETH_P_ALL` tap can name that VID (`222`); the protocol-bound socket reads `none` |

B and E decide the whole shape. **E** is why an unconfigured VLAN is visible at all — a box
on a VID nobody created can be heard, which is what makes creating it possible. **B** is the
trap: the parent receives every sub-interface's frames with the tag gone, so a daemon that
enumerated interfaces naively would run two listeners on one box's frames and the parent's
listener, having no tag, would answer UNTAGGED onto the native VLAN. Two masters for one box,
arrived at through a kernel behaviour rather than a second process.

**THE DETECTOR (`src/reac_topo.{h,c}`).** One `ETH_P_ALL` socket per physical parent,
`SO_ATTACH_FILTER`-ed to `0x8819` plain or behind up to two tags, `PACKET_AUXDATA` on,
read-only, never transmitting. It must not be a protocol-bound socket: fact B is that such a
socket cannot tell a tagged frame from an untagged one, so a detector built on the obvious
socket **would report every trunk as an access port**. The BPF filter is not decoration — an
unfiltered `ETH_P_ALL` socket on a trunk copies every frame on the link to userspace. The
classifier is pure and takes both shapes a kernel can hand it, the accelerated tag (metadata,
what this kernel does) and an in-buffer tag (0x8100/0x88a8 in the bytes), because which
arrives is a driver's business; on QinQ the OUTER VID wins, since that is the one that names
the netdev. VID 0 is a priority tag, names no VLAN, and mints nothing.

**IT ONLY EVER UPGRADES, so it needs no dwell.** Untagged REAC with no sub-interfaces is an
ordinary segment and is today's behaviour; any tagged REAC means a VLAN exists and needs one.
There is no moment at which the daemon must conclude "this is not a trunk", so there is no
window to tune and no timer to get wrong. A box that first speaks on VID 12 an hour into the
show is served an hour into the show.

**ADOPT OR CREATE, AND ONLY WHAT WE MINTED IS EVER REMOVED (`src/reac_vlan.{h,c}`).** Having
heard VID N on parent P, the daemon needs `P.N`:

| the netdev | what happens |
|---|---|
| absent | CREATED over rtnetlink (`IFLA_LINKINFO` kind `vlan`, `IFLA_VLAN_ID`), marked `reac-pw:minted` in its `IFLA_IFALIAS`, brought up — and it is OURS |
| present, no alias of ours | ADOPTED untouched. It is the host's: brought up if it is down, never reconfigured, never removed |
| present, carrying `reac-pw:minted` | a LEAKED MINT from a previous unclean exit: adopted for use AND re-owned, so the next clean exit removes it |

The mark rides the object because it needs no second store — no state file to go stale, no
PID file to reconcile after a crash — and it lives exactly as long as the netdev it
describes. Without it the naive design silently converts a leak into an adoption: it exists
at the next start, so it is adopted, so it is never removed, and the leak becomes permanent
and invisible. The alias is written in the message right after the create, because the
kernel honours `IFLA_IFALIAS` on its setlink path and not on its create path — a create that
carried the alias would quietly produce an UNMARKED netdev, which is the leak this exists to
prevent.

A minted netdev is removed on a clean exit, and when its VID has carried nothing for
`REAC_TOPO_SILENCE_HOLD_NS` (30 s — comfortably past the segment hold and any box
power-cycle, because removing a netdev drops a segment). An adopted one is left exactly as
it was found, in both cases.

**A PARENT CARRYING TAGGED REAC IS NEVER ITSELF DRIVEN.** The moment tagged REAC is heard on
a parent it stops being a candidate segment: a served listener on it is dropped and a fresh
serve is refused, both by name in the journal. This is fact B enforced. And §4f's one
refusal follows from the same place — untagged REAC on a parent that also carries tagged
REAC is **refused, not served**: *REAC on a trunk's native VLAN is not served; give it a
tag.* Driving it would put a master on the parent while masters run on its sub-interfaces.

**THE PLAIN UNTAGGED NIC IS NOT A SPECIAL CASE AND DOES NOT GO AWAY.** A physical interface
that hears untagged REAC and carries no tagged REAC is a segment exactly as it was in 0.5.2,
through exactly the same code. The direct-cable rig this daemon runs on today is unchanged by
this release, and the veth proof asserts that in the same run as the trunk phases.

**THE ADMIN HALF — what happens with no `CAP_NET_ADMIN` (§4e, and the unit already grants
it).** The RPM's `%caps` line is `cap_net_raw,cap_net_admin,cap_sys_nice=ep`, and the startup
preflight has named CAP_NET_ADMIN since 0.5.0. Where it is absent — a hand-built binary that
missed the setcap, a binary on a `nosuid` filesystem, which strips file capabilities with no
error at all — the rule is **report, never fail deaf**:

- the daemon starts, and every interface it can hear it goes on hearing;
- each VID it cannot serve is named once, with the `ip link add link <parent> name
  <parent>.<vid> type vlan id <vid>` that would fix it. A daemon that says *"I can see four
  VLANs and cannot use them"* has done the hard part;
- **adoption needs no capability**, so a host that pre-created its sub-interfaces is fully
  served by an unprivileged daemon;
- the ensure is retried on a window (`REAC_TOPO_RETRY_NS`, 10 s) rather than at wire speed,
  so the refusal costs one line and not one line per frame.

**WHAT THE JOURNAL SAYS**, and these are the lines an operator greps for:

```
[enp131s0] tagged REAC heard — vid 11 (1 frame): this parent is a TRUNK, its VLANs are the segments
[enp131s0] vid 11: created enp131s0.11 (marked reac-pw:minted) — serving it as a segment
[enp131s0] vid 12: adopted enp131s0.12 — the host made it, it survives us
[enp131s0] vid 13: re-owned enp131s0.13 — it carries our mint alias, so a previous run leaked it
[enp131s0] this parent carries tagged REAC, so it is not itself a segment — its VLANs are
[enp131s0] untagged REAC on a trunk's native VLAN is not served; give it a tag
[enp131s0] vid 14: enp131s0.14 cannot be created (Operation not permitted) — CAP_NET_ADMIN
[enp131s0.11] removed — we created it, so we take it away
```

**WHAT IS PROVEN, AND WHERE.** `tests/test_reac_topo.c` holds the classifier (both kernel
shapes, QinQ, VID 0, the untagged copy of fact B) and the netdev lifecycle table.
`tests/hearing-finds-a-segment.sh`'s trunk phases are the job: a peer sends REAC tagged with
two VIDs on ONE veth, the daemon creates two sub-interfaces, serves both as segments with a
box on each and both nodes on the graph, refuses the parent by name, ADOPTS a sub-interface
that was already there, and on exit deletes what it minted and leaves what it adopted.

**WHAT THIS RELEASE DOES NOT DO.** The VIDs are learned per parent and the netdevs are made;
nothing measures the link budget, so a trunk offered more VLANs than a gigabit can carry is
served until it is not (§12a's bound is about twenty at 48 kHz, eight recommended at 96 kHz).
And no rig has yet been on a trunk: this is proven on veth, and the rig proof of §16's
increment 4 — two boxes on two VLANs of one NIC — is owed.

## 0.5.4 — a desk that turns up second takes the wire, and the pace tells the truth (2026-09-09)

**Operator ruling: the yield is not about how we won the wire, it is about whether the
wire was ours to lose.** 0.5.0 yielded only a segment taken on PROVEN SILENCE, because
driving that one was a bet. The venue case says the distinction does not survive contact:
a house console is powered, hears the stageboxes on a wire nobody pinned, grants them and
drives — and then the Roland desk is switched on. That wire was won on EVIDENCE, so 0.5.0
kept mastering it and published a conflict nobody could act on. Two masters on one segment
is the fault the seglock exists to make impossible between our own processes, and it is no
better against a desk.

So the rule is one line, and it names the wire rather than the reason:

| the wire | what a foreign master arriving does |
|---|---|
| unpinned, taken on proven SILENCE | yielded to — unchanged since 0.5.0 |
| unpinned, taken because a BOX was heard on it | **yielded to** — new here |
| unpinned, JOINED to somebody from the start | nothing to yield; we never had it |
| pinned MASTER | the pin stands; a box mastering it is the 0.5.1 refusal, and a desk is the conflict the props already carry |

**This answers the arbitration spec's Q1 for unpinned segments, and it costs something.**
Yielding drops a box mid-audio: the master engine goes down, the slave engine comes up,
and the box's stream stops for the length of the swap. Holding costs the whole segment
instead — a desk that will not be argued with drives the boxes anyway, and what we would
be defending is a second master on its wire. The operator ruled for the yield, on a wire
nobody answered for; a wire the operator DID answer for still keeps its answer.

**And a yield is not a one-way door.** The desk goes away — powered off at the end of the
night, a cable pulled — and the segment must come back rather than sit slaved to a wire
nobody is driving. The sniffer is therefore kept ACROSS the yield, not just up to it: when
the rival's sighting ages out of the discovery table (`REAC_DISCO_STALE_NS`, 5 s, the same
bar "a device is really gone" means everywhere else in this daemon) the verdict returns to
MASTER and the wire is taken again, through the same drop-then-serve seam a cold start
uses. The masterless licence granted on that wire at t0 is not revoked by a rival that came
and went, and nothing latches in either direction: the desk coming back yields again.

The journal says which way it moved, one line per transition: `a desk masters this segment
… yielding the master role and joining as SLAVE`, and `the desk stopped mastering this wire
… taking the segment back as MASTER`.

**`reac.pace.source` publishes the pacer's own reference, and it used to publish a
constant.** The playback door built its arbitration with `REAC_PACE_FREE_RUN` written in,
from a 2026-08-21 config of record in which clock-follow was off. Following has been the
DEFAULT since 0.5.0, and on the rig 2026-09-08/09 the journal read `locked to graph clock
(api.alsa.0)` while the console's segment row read `free-run` — a published fact
contradicting the daemon that published it. The pacer thread now mirrors its discipline
(source + state) into one atomic beside the event it already pushes on every change, and
the door maps that to the vocabulary `reac_arbitration.h` already owns:

| the pacer's discipline | `reac.pace.source` |
|---|---|
| LOCKED to a NIC/external PHC | `phc` |
| LOCKED to the hardware-driven graph clock | `graph-ref` |
| LOCKED to the box's counter slope | `box-slope` |
| UNLOCKED, LOCKING or HOLDOVER, or follow disabled | `free-run` |

Only LOCKED names a reference. LOCKING is a claim about the future and HOLDOVER is a frozen
period nothing is currently steering — both run on `CLOCK_MONOTONIC` at this instant, which
is what `free-run` means. A foreign master still overrides all of it (`foreign-master`): what
we would have disciplined to is not what the wire is running on.

**WHAT THE RETAKE DOES NOT DO, measured rather than assumed.** It does not re-enrol the
boxes by itself. A REAC stagebox leaves BOOT for ANNOUNCE on ITS OWN PHY-up edge and on
nothing else (`reac_linkmon.h`, #95), so a box that was enrolled with us, sat through the
desk's visit and never dropped has no reason to cold-connect when we come back: the veth
peer's transcript ends at ESTABLISHED and stays there, and our master engine probes. What
the retake owes it is a master that is DRIVING when it does re-announce, and that is what
the proof measures on the peer's own capture. The segment's nodes come back with the BOX
too, not with the role — a master in autodetect sizes `reac-capture`/`reac-playback` from
the box it recognizes — so a retaken segment publishes no node until one enrols. Both are
the standing rules of every master take; neither is new here, and neither is hidden.

**TWO LEAKS THE PROOF FOUND, both bounded at 8 and both silent.** A SERVED interface goes
LISTEN → SERVE → DROP and never through UNLISTEN, which was the only path that gave back a
topology tap — so every segment that ever dropped kept its tap AND its row in the topology
table for the life of the process. Past the eighth, `no room for a topology tap` and every
trunk after it is served as one flat segment; the table's own refusal said nothing at all.
The DROP path now returns both, and a full table is reported like a full tap array. Found
because the venue phase was the ninth interface of the veth run and the trunk phase stopped
seeing vid 13.

**AND A SUB-INTERFACE IS NEVER TAPPED, WITHOUT ASKING SYSFS.** `reac_topo_is_stacked` reads
`/sys/class/net/<if>/lower_*` and answers NOT STACKED when the path cannot be read, which is
the same answer an ordinary NIC gives — a fail-open on a topology action. In the proof's
namespace `/sys` is the host's, so once tap slots were free again the daemon tapped its own
`trunk1.13`, read vid 13 out of the tag the kernel had just stripped for it, and minted
`trunk1.13.13`. It does not need sysfs: `<parent>.<vid>` is the name the daemon uses itself,
so a netdev whose prefix up to the last dot is a parent in the topology table IS that
parent's sub-interface, and a physical NIC can never match.

**WHAT IS PROVEN, AND WHERE.** `tests/test_reac_watch.c` holds the re-decision table —
which served segments keep being classified, and which of yield, retake, unrefuse or stand
a fresh verdict means. `tests/test_reac_arbitration.c` holds the pure map from
(source, state) to the published word, including that every non-LOCKED state reads
`free-run`. `tests/test_reac_pacer_clock.c` drives the real discipline to lock and asserts
the pacer reports it — the accessor over the thread boundary, which is where the constant
was. `tests/hearing-finds-a-segment.sh`'s venue phase is the job, end to end: the daemon
takes a masterless wire and a cold box answers it and is enrolled; a fake desk starts
announcing on the SAME wire, and within one announce cadence the daemon is that desk's
slave — the master door is off the graph and our broadcasts stop, measured on the peer's
own capture against a live control; the desk is then switched off, and after the hold the
daemon takes the wire back and is measured DRIVING it again. The same phase reads
`reac.pace.source` off the two nodes a console reads it from and requires it to say what
the daemon's own clock transcript says — under a private PipeWire with no hardware to
offer, which never locks, so the agreement is what is proven there and the LOCKED mapping
is proven above it.

## Files

| File | Role |
|---|---|
| `src/main.c` | CLI + lifecycle: parse `--role` (no `--box` — the master's box is learned from the wire), open feeder, create source node, then (master) the sink or (slave) the slave engine; run the loop |
| `src/reac_role.h` | **role selection**: `--role master\|slave` parse + validation (slave requires `--tx`), header-only + unit-tested |
| `src/reac_topo.{h,c}` | **the trunk detector**: the pure 802.1Q classifier (PACKET_AUXDATA or an in-buffer tag), the per-parent VLAN table with its ensure/release lifecycle, and the read-only `ETH_P_ALL`+BPF tap that feeds them |
| `src/reac_vlan.{h,c}` | **the netdevs behind it**: `<parent>.<vid>` created over rtnetlink and marked `reac-pw:minted`, adopted where the host made it, removed only where we made it |
| `src/reac_ring.{h,c}` | lock-free SPSC planar-float ring (RX hot-path → process(); also the slave's upstream-input carrier) |
| `src/reac_rx.{h,c}` | non-RT feeder: wire source (live/pcap) → libreac validate → role-gated decode (downstream 40-ch / upstream box return) → f32 → ring; counter-slope ppm estimator |
| `src/reac_source_node.{h,c}` | `reac:capture` pw_filter: 40 F32 ports, RT process(), follower/driver clock (RX for BOTH roles) |
| `src/reac_tx.{h,c}` | raw-socket AF_PACKET emitter + `reac_eth_crc32` (the OHRCA-trailer RE verifier). The frame encoder moved to libreac 2026-07-29 (`reac_downstream_build`, `<reac/reac_encode.h>`) |
| `src/reac_master.{h,c}` | **master-role** JOIN/HOLD: the cdea/cfea establishment FSM + captured control-block templates (S2) |
| `src/reac_pacer.{h,c}` | **master-role** SCHED_FIFO cadence pacer + TX frame ring; stamps the master block on egress (S6) |
| `src/reac_sink_node.{h,c}` | `reac:playback` Audio/Sink: process() encodes + submits to the pacer (the master TX) |
| `src/reac_ctrl.{h,c}`, `src/reac_fsm.{h,c}` | the slave control plane: virtual-stagebox builders/parser/checksum + the pure JOIN/HOLD FSM |
| `src/reac_slave.{h,c}` | **slave-role** engine: drives `reac_fsm` from RX events, locks to the master cadence, returns our inputs upstream (S7) |
| `src/reac_master_fsm.{h,c}` | the master establishment decisions as a PURE `(state, event) → (state, entry action, drop)` table — the shape `reac_fsm.h` gave the slave side (#61); spec = [docs/MASTER-FSM.md](docs/MASTER-FSM.md) |
| `src/reac_slots.h` | the TWO slot spaces, named once with their capture evidence: AUDIO fabric 40 vs HEAD-AMP/chanmap 48 (`0x00..0x2f`). Never one for the other (#69) |
| `src/reac_boxreg.{h,c}` | the multi-box registry: box MAC → (base, nch, name) over the 40-slot AUDIO fabric, allocated by first-JOIN order or pre-declared |
| `src/reac_grant.{h,c}` | the master's per-channel ENROLLMENT SWEEP (the `cdea 04 03` grant): group A head-amp records + group B, over the HEAD-AMP space |
| `src/reac_headamp_tx.{h,c}` | **master-role** head-amp SEND model: edge on change + the complete scene at every establishment, plus a periodic re-assert of the SET cells that is built and SHIPPED DISABLED ([docs/HEADAMP-REASSERT-POLICY.md](docs/HEADAMP-REASSERT-POLICY.md)) |
| `src/reac_headamp_prop.{h,c}` | parse live head-amp changes out of `SPA_PARAM_Props` (`reac.headamp.<ch>.<phantom\|pad\|sens>` over `SPA_PROP_params`) |
| `src/reac_clock.{h,c}` | the clock-discipline core: role-dependent reference hierarchy, quality grading, and a bounded period DLL. ON by default since 0.5.0; INERT under `REACPW_CLOCK_FOLLOW=0` (#75/#77) |
| `src/reac_gain.{h,c}` | pure RT-safe output-gain staging for `reac:playback` (linear `SPA_PROP` volume/mute, ramped) |
| `src/reac_lat.{h,c}` | `ProcessLatency` smoothing for the sink: the pacer ring depth sawtooths, the advertised contract must not (#152) |
| `src/reac_link_state.{h,c}` | pure mapping from the master FSM state onto the node-property badge a consumer (openmixer's stagebox card) reads |
| `src/reac_link.{h,c}` | **is there a CABLE** — a dependency-free `/sys/class/net/<if>/carrier` predicate, 1/0/-1 UNKNOWN. Read by the PROBING watchdog so "the box is silent" and "the cable is out" stop reading the same (#95). Answers UNKNOWN for an admin-down interface, which the file cannot describe |
| `src/reac_ifscan.{h,c}` | **WHICH interfaces to listen on, and which are segments** — the host's netdev table over rtnetlink, one decision per Ethernet interface. Link is the gate to LISTEN (a passive 0x8819 sniffer, `main.c`'s hearing supervisor), the first REAC frame heard is the gate to SERVE, and link loss drops the segment after a 3 s hold a box power-cycle cannot outlast; `RTM_DELLINK` and a re-enumerated ifindex drop at once. A segment is named after its interface; nothing names one in advance (openmixer's trunk-VLAN spec, amendment 2026-09-02). Pure table + event queue, netlink as a byte source, same shape as `reac_linkmon` |
| `src/reac_ifname.{h,c}` | a segment's STABLE, bus+physical-address-derived name (`pci1`, `usb2`) — built, tested against real captured `/sys` paths, and DELIBERATELY NOT WIRED into segment identity. A segment IS its interface here and is NAMED after it, and a console generates its per-segment keys and its patch addresses from that published name — so swapping the identity renames every key and every patch on a live rig in one step. The answer to name instability is node names that follow the BOX (owed, see "What 0.5.0 does not do"), not a second name derived from the interface. Kept for that work; wired to nothing today |
| `src/reac_hunt.{h,c}` | **WHICH END OF THE PAIRING A HEARD SEGMENT TAKES**, when nothing was configured — the ACT half over `reac_arbitration`'s passive observation. Sightings accumulate in the discovery table for a 3 s window = three master announce cadences; a desk mastering the wire is joined as a SLAVE at once, a wire with a box on it and no master is taken as MASTER when the window closes, and a stagebox strapped to master is joined as a SLAVE too — at the width its frame geometry declares — unless the operator pinned that segment MASTER, which is the one contradiction the daemon refuses (`rival-master-box`) instead of out-shouting (0.5.1). A `REAC_ROLE_<segment>` pin skips all of it and is served ON LINK, with no frame required — a cold slave box is silent until a master announces to it, so waiting for a classifying frame on a pinned wire waits forever (2026-09-08, both rig boxes mute). Nothing latches: the table ages, and the verdict is recomputed. Pure |
| `src/reac_watch.{h,c}` | **RE-DECIDING A SEGMENT THAT IS ALREADY UP** (0.5.4): which served segments keep their sniffer — every unpinned wire WE took, and every refused one — and what a fresh verdict off it means: YIELD to a master that turns up on a wire we are driving, RETAKE one whose rival has aged out of the discovery table, UNREFUSE a door whose rival stopped mastering it (after the table's own withdrawal window, or it flaps), STAND otherwise. It is the PIN that decides whether a wire is ours to lose, never the evidence we won it with. Pure, because inline in main.c the only thing that could exercise it was a 70 s veth run that cannot choose which route took the wire |
| `src/reac_knock.{h,c}` | **THE PROOF THAT A WIRE HAS NO MASTER ON IT**, which is the licence to drive. A stagebox in slave mode spends a bounded broadcast flood on PHY-up and then goes silent forever if no master answered it, so hearing alone can never wake one that was powered before the daemon (rig, 2026-09-08 22:10: two boxes cabled and carrier-up, zero frames in eight seconds). An unpinned linked WIRED interface is listened to for REAC_KNOCK_LISTEN_NS (500 ms = 1837 slots at the slowest cadence; a master fills every slot, so silence there is PROOF of no master) and then TAKEN as master through the ordinary master role — not knocked on: a lone announce every 2 s was measured on the rig and the box never answered. Any frame inside the window cancels the licence. Pure: one clock, one verdict, no socket and no frame |
| `src/reac_linkmon.{h,c}` | **the cable CHANGING** — an `RTM_NEWLINK` watch on one named interface, reporting edges. A box leaves BOOT for ANNOUNCE on PHY link-up and on nothing else, so that edge is the only instant it enrols; the sink node drives an internal re-establish from it, at the standing rate (#95). Uses `IFF_LOWER_UP`, never `IFLA_CARRIER`: only the flag folds in `netif_running`, and `ip link set <nic> down` must read as a loss |
| `src/reac_disco.{h,c}` | passive segment discovery: what is on this wire, including the frames the master classifier deliberately discards |
| `src/reac_mac.{h,c}` | the stand-in source MAC: Roland OUI + our own NIC's host part, so it cannot collide with a real box |
| `tests/` | 65 meson tests, all offline except `reac_pacer`'s live-cadence case (SKIPs without `CAP_NET_RAW`). `meson test -C build` lists them; the goldens (`reac_conformance_golden.inc`, `reac_grant_golden.inc`, `reac_m200_golden.inc`, `upstream_fixtures.inc`) are real captured bytes and are the oracle — never regenerate one to make a diff go away |
| `meson.build`, `meson_options.txt` | build: pipewire/spa + libreac via pkg-config, libreac subproject fallback; no build options |
| `subprojects/libreac.wrap` + `packagefiles/libreac/meson.build` | libreac as a meson subproject |
