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
- Braided upstream encoder (`src/reac_ctrl.c: place_braided_audio`,
  `reac_ctrl_build_upstream_filler`) — planar float → box-width braided upstream
  frame. Layout verified against rig captures (task #108).
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

**(a) OHRCA 1494 B frame length [V, offline 2026-07-11] — MUST FIX for M-5000.**
The M-5000 (OHRCA, 96 kHz) downstream is **1494 B, not 1492**: a standard REAC
frame (audio `[50:1490]`, `C2 EA` end-marker at `[1490:1492]`) plus a **2-byte
per-frame CRC-16 trailer** at `[1492:1494]` (measured near-unique: 20004 distinct
values / 23759 frames). `gate_accepts` requires `len == REAC_FRAME_BYTES` (1492),
so it **rejects every M-5000 frame — capture ports go silent with no error
logged.** Fix: accept `1492 || 1494`, decode the embedded `[0:1492]` frame as
today, ignore the trailer. The CRC only needs computing if we later *emit* toward
an OHRCA-expecting box (a separate RE task).

**Trailer characterization (offline 2026-07-11, not fully cracked).** The 2-byte
trailer is NOT a checksum of frame content — it varies for byte-identical content
across different counters. It is a **linear (GF(2)) function of the free-running
frame counter**: within every constant-content group,
`trailer_i XOR trailer_j = L * (counter_i XOR counter_j)` for a fixed 16x16
matrix `L`. That is the fingerprint of a **CRC-16 seeded by the frame counter**
(`init = counter`) — a sequence-integrity field, not a data CRC. Standard
constant-init CRC-16 sweeps (all poly/init/refl/xorout over every contiguous
range) plus Fletcher-16 and modular sums all MISS, consistent with the counter
seed. Full polynomial recovery is blocked only by data: the observed counter
differences span 15 of 16 dimensions (one bit short) — a capture that exercises
the 16th counter bit finishes it. Off the RX path (we ignore the trailer);
needed only to EMIT toward an OHRCA-expecting box.

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
