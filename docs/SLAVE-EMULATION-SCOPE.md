# reac-pw SLAVE emulation — scope

**Goal:** reac-pw presents to a real Roland desk (M-200i / M-300 / M-5000) as a
REAC stagebox. The desk's downstream buses become PipeWire capture ports;
PipeWire playback (openmixer / apps) is returned upstream as the box's mic
inputs. This is the clock-*tractable* half of REAC: the desk owns the crystal
and reac-pw slaves to its cadence — the inverse of the master role's unproven
hardware-clock blocker (#131).

**Why slave first (vs master):** the 2026-07-11 mixer-vs-box matrix
(`reac-firmware-re/MIXER-VS-BOX-MATRIX.md`) proved the box side is fully
characterized and that the master-side box-mute is a physical/clock-domain
problem — not L2 content. As a slave, reac-pw *receives* the desk's
hardware-locked cadence and phase-aligns to it (the proven `reac-repacer-clk`
principle), so it sidesteps that blocker while building the same disciplined
pacer master will later need.

## Already implemented (verified in-tree, do NOT re-scope)

- `--role slave` CLI + dispatch (`src/main.c`).
- Establishment FSM (`src/reac_fsm.c`): PHY→FLOOD→COLDCONNECT
  (`0014→0013→0016→001a`)→TX_MUTE→ESTABLISHED→heartbeat/DROP; learns the master
  MAC from the wire. Offline-proven end-to-end by `tests/test_reac_courtship.c`.
- Braided upstream encoder (libreac's `reac_braid_encode`, driven by
  `src/reac_ctrl.c: reac_ctrl_build_upstream_filler`) — planar float → box-width
  braided upstream frame. Layout verified against rig captures (task #108); the
  braid loop itself moved to libreac 2026-07-29 (it was the same loop as the
  downstream encoder's), the frame envelope + control block stayed here.
- Downstream RX → PipeWire source node (`src/reac_source_node.c`,
  `src/reac_rx.c`) — the master's audio decoded to capture ports.

## Work items

### W1 — PipeWire `reac:return` sink for slave role  ·  effort M  ·  **#1 gap**
`main.c` builds a `reac_sink_node` only for the master role. In slave role
`tx_ring` is created but nothing fills it, so the encoder runs on silence (see
the `main.c` slave-branch comment). Add a slave-side sink node that accepts
PipeWire playback into `tx_ring`; the slave engine already drains it through the
braided encoder. Delivers "inject audio as the box's mic inputs."
Files: `src/reac_sink_node.*` (reuse/param), `src/main.c` slave branch.

### W2 — Frame-locked upstream TX (clock recovery)  ·  effort M–L  ·  clock heart
The slave must emit each upstream frame phase-aligned to the master's slot (a
real box answers ~69 µs after each downstream frame), driven by RX frame
arrival + the master's counter — never a free-running pacer. Verify
`reac_slave` TX is anchored to received cadence; re-anchor if it free-runs
(cumulative-lossless-count law, per `reac-repacer-clk`). Tractable because the
desk owns the clock: "recover + align," not "generate a crystal."
Files: `src/reac_slave.c`, `src/reac_rx.c` (arrival hook).

### W3 — Configurable box width + identity  ·  effort S
Present as a chosen box via `--box-channels {8,16,32}`; set config-announce
(`01030010`) width and the per-unit `0014 [20]/[22]` field to match a plausible
box. Currently hardcoded to `REAC_SLAVE_BOX_CHANNELS_DEFAULT`.
Files: `src/main.c`, `src/reac_slave.c`, `src/reac_ctrl.c`.

### W4 — Downstream decode: OHRCA frame length + per-generation layout  ·  effort S–M
Two distinct issues found in `src/reac_rx.c`:

**(a) ⚠ FALSIFIED (2026-07-12): the "+2 CRC-16 trailer" was the ETHERNET FCS.**
The earlier claim — that the M-5000 (OHRCA) frame is 1494 B = a 1492 B REAC frame
plus a 2-byte per-frame CRC-16 trailer (and the box upstream 1206 B = 1204 + 2) —
is **WRONG**. Proven: the 2 "trailer" bytes are exactly the **first 2 bytes of the
standard Ethernet FCS** (`CRC-32(frame[0:L])`, little-endian) — verified on the
real S-4000S (`s4000s-coldboot-m5000`, box `c4:06:80`): `trailer = ff93` equals
`CRC32(frame[0:1204]) = ff93 95e4`, exactly, on every frame tested. Some switch
mirror/SPAN configs include a couple of FCS bytes in the captured frame; others
strip them — which is why the SAME box on the SAME desk showed 1206 B in one
capture and the correct 1204 B in another. The box's real REAC frame is the
constant `box_frame_len(n)` length, ending in `C2 EA`, with **no REAC trailer**.
Consequence: there is **nothing to crack and nothing to emit** — the Ethernet FCS
is computed by the NIC hardware, so reac-pw's frames already carry a valid one.
Do NOT reintroduce a "per-frame CRC-16 trailer" gate or emitter.

**Why it masqueraded as a counter-seeded CRC-16.** The Ethernet FCS is a CRC-32
over the whole frame *including the counter field*, so 2 of its bytes are a linear
(GF(2)) function of the counter for constant content — which is exactly the
"trailer_i XOR trailer_j = L·(counter_i XOR counter_j)" fingerprint we mistook for
a bespoke counter-seeded CRC-16. Standard CRC-16 sweeps missed because it was never
a CRC-16; it was 2 bytes of the CRC-32 FCS. Lesson: verify capture ground-truth
(mirror/SPAN byte-faithfulness) before RE-ing a "trailer".

**(b) Per-generation audio layout.** `reac_rx` decodes downstream via
`reac_decode` = **plain-LE**, which is CORRECT for the M-5000 (OHRCA) but WRONG
for M-200/M-300 (they braid the downstream → would decode as noise). No change
needed for the M-5000; add a per-generation switch/autodetect before targeting a
V-Mixer desk. Wrong layout = silent noise, so wire-verify.

Files: `src/reac_rx.c` (`gate_accepts`, `feed_frame`).

### W5 — On-wire validation against a real master  ·  rig time  ·  the proof
`test_reac_courtship.c` only proves our-slave ↔ our-master. Prove a real desk
GRANTs our cold-connect, reaches ESTABLISHED, streams to us, and shows our
upstream on its input meters. Box templates are fully RE'd (low risk) but this
is the milestone that counts.

## Build order (each ends in a wire test)

1. **W4** — RX the live desk's downstream, confirm clean audio in PipeWire.
   *Milestone: desk bus output appears clean in capture ports.*
2. **W3 + W5 handshake** — real desk GRANTs our slave, holds ESTABLISHED.
   *Milestone: desk shows the box synced; heartbeat holds.*
3. **W1** — wire the return sink, inject a test tone.
   *Milestone: tone shows on the desk's input-channel meter.*
4. **W2** — tighten phase-lock. *Milestone: clean audio on the desk input over
   minutes, no slips.*

## Open risks
- **W4 layout** — silent-noise failure mode if the per-generation layout is wrong.
- **W5 reciprocal handshake** — desk granting our slave is unproven on the wire.
- **W2 phase-lock** — the clock piece; tractable (desk owns clock) but slips
  must be avoided (downstream frame-slip injects a 12-sample phase step).

## W5 CONNECTED (2026-07-11): real M-200 enrolls reac-pw in its REAC menu ✅

**Milestone reached.** reac-pw ran as a 16-ch S-1608 slave against a real,
cold-booted **M-200** (V-Mixer, 48 kHz) and the desk **showed it as a connected
stagebox in the REAC menu** and held the link across a 300 s run. Wire census
(`reacpw-slave-m200-CONNECTED-2026-07-11.pcap`): master `GRANT` burst at the
cold-connect step → **PROBE 0/s** for the whole run → steady **3999** fill/s both
directions, CHANMAP + heartbeat ~0.5/s. No re-hunt, no drop.

This closes W5 **for V-Mixer desks (M-200/M-300/M-200i)** and **falsifies the
"clock-domain wall" reasoning below**: reac-pw paces off `CLOCK_MONOTONIC`, yet a
real desk accepts it as a settled box. The M-5000 (OHRCA) is still open, but the
cause is now understood to be the **OHRCA established-state shape** (1494 B frame +
per-frame CRC-16 trailer, 96 kHz upstream — see W4) that reac-pw does not yet
emit, NOT a hardware crystal requirement. Remaining slave work for full M-5000
support: emit the OHRCA-width upstream + CRC-16 trailer, then re-test.

## W5 live result (2026-07-11): GRANTED — the missing frame was the config-announce

**RESOLVED. The earlier "clock domain" conclusion below was WRONG** — it came
from a flawed comparison that byte-diffed only the frame types present in BOTH
captures, so a frame reac-pw never sent (the box's setup declaration,
`cdea 01 03 0010`) was invisible to the diff. A real S-1608 sends a
config-announce during cold-connect; the master ENROLLS the box from it. reac-pw
never sent it, so the M-5000 never registered the box.

Fix: `reac_ctrl_build_config_announce` now emits the real S-1608 setup block
(byte-matched), and the slave cycles it (plus an early heartbeat) into the
cold-connect escalation. **Live result: the M-5000 emitted 24 grant frames
(`04030013`) and enrolled reac-pw** (2026-07-11). A phase-matched full-lifecycle
census then confirmed reac-pw emits the COMPLETE frame set a real S-1608 sends
(FLOOD, config-announce, `0014/0013/0016/001a`, heartbeat, filler) — every type
byte-IDENTICAL.

**Methodology lesson:** to assert "we send the same," compare the COMPLETE set of
emitted frame TYPES (presence/absence) first, then byte-diff — never diff only
the intersection, which hides missing frames.

---

### (superseded) earlier clock-domain reasoning

Ran reac-pw as a slave against a live M-5000 (96 kHz) on `enp131s0` (the clean
REAC NIC — receives the desk's downstream with single, non-double-counted
counters). Systematically eliminated every wire-observable difference vs a real
S-1608:

- **Full establishment on the wire [V]:** presence flood (5460 broadcast frames,
  matching a real box), the complete cold-connect escalation `0014→0013→0016→001a`
  (added this session), 8000 pps / 628 B braided upstream, RX-locked phase.
- **Byte-identical to a real S-1608 [V]:** diffed reac-pw's emission against a
  fresh real-S-1608 cold-boot on the same rig
  (`real-s1608-coldboot-m5000-2026-07-11.pcap`). Flood block, unicast-filler
  descriptor, and all four cold-connect control blocks came back **IDENTICAL**.
- **Phase matches [V]:** real S-1608 answers ~0.9 µs after each downstream frame,
  reac-pw ~2.8 µs — both effectively immediate (the earlier ~69 µs figure was
  wrong). Phase is NOT the discriminator.
- **MAC is not it [V]:** impersonating the real box's exact MAC
  (`--src-mac 00:40:ab:c4:80:3b`, box unplugged) — still no grant.
- **Delivery proven [V]:** the M-5000's port mirror shows 23559 of reac-pw's
  frames, so the desk physically receives them.

**Conclusion:** the M-5000 grants a real S-1608 (216 grant frames observed) but
never grants reac-pw, despite receiving byte-identical frames at the same rate,
phase, and MAC. The discriminator is invisible to packet capture — it is the
physical/clock domain: a real box clocks its frames from an FPGA audio crystal
locked to the sample clock; reac-pw paces from `CLOCK_MONOTONIC`. This is the
reciprocal confirmation of the mixer-vs-box matrix conclusion (the master-side
box-mute is the same wall). The path forward is #131 — DLL-discipline the pacer
to a real hardware word clock (e.g. RME) — which is hardware-dependent, not a
pure-software fix.
