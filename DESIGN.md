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
`reac.box-source` (`wire` while a box is known, `none` otherwise) and
`reac.headamp.base` — the head-amp wire channel the box's input 1 sits at, i.e. the
`base` in `CH = base + (input - 1)`. `reac_link_state.h` defines all of them.

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

## Files

| File | Role |
|---|---|
| `src/main.c` | CLI + lifecycle: parse `--role` (no `--box` — the master's box is learned from the wire), open feeder, create source node, then (master) the sink or (slave) the slave engine; run the loop |
| `src/reac_role.h` | **role selection**: `--role master\|slave` parse + validation (slave requires `--tx`), header-only + unit-tested |
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
| `src/reac_headamp_tx.{h,c}` | **master-role** head-amp SEND model: the declarative/DMX table the master re-asserts (full table on a slow period + an edge record on change) |
| `src/reac_headamp_prop.{h,c}` | parse live head-amp changes out of `SPA_PARAM_Props` (`reac.headamp.<ch>.<phantom\|pad\|sens>` over `SPA_PROP_params`) |
| `src/reac_clock.{h,c}` | the clock-discipline core: role-dependent reference hierarchy, quality grading, and a bounded period DLL. INERT unless `REACPW_CLOCK_FOLLOW` (#75/#77) |
| `src/reac_gain.{h,c}` | pure RT-safe output-gain staging for `reac:playback` (linear `SPA_PROP` volume/mute, ramped) |
| `src/reac_lat.{h,c}` | `ProcessLatency` smoothing for the sink: the pacer ring depth sawtooths, the advertised contract must not (#152) |
| `src/reac_link_state.{h,c}` | pure mapping from the master FSM state onto the node-property badge a consumer (openmixer's stagebox card) reads |
| `src/reac_link.{h,c}` | **is there a CABLE** — a dependency-free `/sys/class/net/<if>/carrier` predicate, 1/0/-1 UNKNOWN. Read by the PROBING watchdog so "the box is silent" and "the cable is out" stop reading the same (#95). Answers UNKNOWN for an admin-down interface, which the file cannot describe |
| `src/reac_ifscan.{h,c}` | **WHICH interfaces to listen on, and which are segments** — the host's netdev table over rtnetlink, one decision per Ethernet interface. Link is the gate to LISTEN (a passive 0x8819 sniffer, `main.c`'s hearing supervisor), the first REAC frame heard is the gate to SERVE, and link loss drops the segment after a 3 s hold a box power-cycle cannot outlast; `RTM_DELLINK` and a re-enumerated ifindex drop at once. A segment is named after its interface; nothing names one in advance (openmixer's trunk-VLAN spec, amendment 2026-09-02). Pure table + event queue, netlink as a byte source, same shape as `reac_linkmon` |
| `src/reac_linkmon.{h,c}` | **the cable CHANGING** — an `RTM_NEWLINK` watch on one named interface, reporting edges. A box leaves BOOT for ANNOUNCE on PHY link-up and on nothing else, so that edge is the only instant it enrols; the sink node drives an internal re-establish from it, at the standing rate (#95). Uses `IFF_LOWER_UP`, never `IFLA_CARRIER`: only the flag folds in `netif_running`, and `ip link set <nic> down` must read as a loss |
| `src/reac_disco.{h,c}` | passive segment discovery: what is on this wire, including the frames the master classifier deliberately discards |
| `src/reac_mac.{h,c}` | the stand-in source MAC: Roland OUI + our own NIC's host part, so it cannot collide with a real box |
| `tests/` | 54 meson tests, all offline except `reac_pacer`'s live-cadence case (SKIPs without `CAP_NET_RAW`). `meson test -C build` lists them; the goldens (`reac_conformance_golden.inc`, `reac_grant_golden.inc`, `reac_m200_golden.inc`, `upstream_fixtures.inc`) are real captured bytes and are the oracle — never regenerate one to make a diff go away |
| `meson.build`, `meson_options.txt` | build: pipewire/spa + libreac via pkg-config, libreac subproject fallback; no build options |
| `subprojects/libreac.wrap` + `packagefiles/libreac/meson.build` | libreac as a meson subproject |
