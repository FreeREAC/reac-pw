# reac-pw SLAVE emulation — scope

**Goal:** reac-pw presents to a real Roland desk (M-200i / M-300 / M-5000) as a
REAC stagebox. The desk's downstream buses become PipeWire capture ports;
PipeWire playback (openmixer / apps) is returned upstream as the box's mic
inputs. This is the clock-*tractable* half of REAC: the desk owns the crystal
and reac-pw slaves to its cadence — the inverse of the master role's unproven
hardware-clock blocker (#131).

**Why slave first (vs master) — the original rationale, since falsified.** The
2026-07-11 mixer-vs-box matrix (`reac-firmware-re/MIXER-VS-BOX-MATRIX.md`) proved
the box side is fully characterized, and concluded that the master-side box-mute was
a physical/clock-domain problem rather than L2 content. Slaving first would sidestep
that blocker, since a slave *receives* the desk's hardware-locked cadence.

⚠ The blocker was not a clock. The master role now locks a real S-0808 and S-1608
SOLID from `CLOCK_MONOTONIC` pacing; the gap was the PROTOCOL — cfea box-count, the
missing ENROLL, the byte-exact grant sweep, grant self-complete + HOLD, the box
heartbeat ([`REAC-MIXER-PROTOCOL.md`](REAC-MIXER-PROTOCOL.md), "Rigorously RULED
OUT: TX timing jitter"). The reciprocal falsification happened on this side too — a
real M-200 enrols our `CLOCK_MONOTONIC`-paced slave (see W5 CONNECTED below). #131
survives only as a fidelity item, not a blocker. The slave-first ORDER was still
right, and the work below still stands; only its stated reason does not.

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

### W3 — Configurable box width + identity  ·  **DONE**
Was: "currently hardcoded to `REAC_SLAVE_BOX_CHANNELS_DEFAULT`". It is not — that
constant is only the default when no flag is given (`src/main.c:201`). `--box-channels N`
(even, 2..40) sets the width directly, and `--box-model {s1608,s0808,s4000s}` picks a
whole FIXED-matrix row — selector, ASCII name frame, `0402000d`, descriptor and width
in one choice (`src/main.c:258-278`, `BOX_MODELS` at `src/reac_ctrl.c:237`). The width
threads through every builder: `reac_slave.c` passes `s->box_channels` into
`reac_ctrl_build_config_announce` and each cold-connect variant
(`src/reac_slave.c:258-324`).

Model rows are the law here, not a knob — see the fixed model matrix in
[`REAC-BOX-STATE-DIAGRAM.md`](REAC-BOX-STATE-DIAGRAM.md), all three rows
live-verified on a real M-200 (2026-07-12). There is no "S-1608 with 8 channels".

### W4 — Downstream decode: OHRCA frame length + audio layout  ·  **(b) CLOSED, (a) OPEN**
Two distinct issues found in `src/reac_rx.c`. The layout half (b) is closed —
one braid, every generation. The frame-length half (a) is still open, but nothing
in the code turns on it.

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

⚠ **NOT SETTLED — read the paragraph above as one side of an open question
(2026-07-29).** Two records in the tree disagree about the DOWNSTREAM `+2` and
neither has been retired. libreac's `<reac/reac.h>` documents
`REAC_FRAME_BYTES_OHRCA` as a REAL 2-byte per-frame OHRCA trailer in **both**
directions, citing live M-5000 downstream captures from 2026-07-11 — the day
before the falsification above. This section and
[`MASTER-HARDWARE-VERIFY.md`](MASTER-HARDWARE-VERIFY.md) read the same downstream
pair as Ethernet FCS bytes leaked in by a mirror/SPAN tap. The **upstream** half
is no longer in dispute and cuts against the paragraph above as written: the
S-4000's 1206 B returns were confirmed on 2026-07-25 to carry a real trailer
(interleaved with ~1/8 trailerless 1204 B frames, which an FCS cannot produce) —
see [`REAC-BOX-STATE-DIAGRAM.md`](REAC-BOX-STATE-DIAGRAM.md), which already
corrects the "box upstream 1206 B = 1204 + 2 is WRONG" clause. The downstream
reading is being re-checked against the captures; nothing in the code depends on
the answer (the decode ignores the 2 bytes either way), so do not act on either
side until it lands. Tracked on #80.

**Why it masqueraded as a counter-seeded CRC-16.** The Ethernet FCS is a CRC-32
over the whole frame *including the counter field*, so 2 of its bytes are a linear
(GF(2)) function of the counter for constant content — which is exactly the
"trailer_i XOR trailer_j = L·(counter_i XOR counter_j)" fingerprint we mistook for
a bespoke counter-seeded CRC-16. Standard CRC-16 sweeps missed because it was never
a CRC-16; it was 2 bytes of the CRC-32 FCS. Lesson: verify capture ground-truth
(mirror/SPAN byte-faithfulness) before RE-ing a "trailer".

**(b) ✅ CLOSED (2026-07-29, #80): the downstream decodes the BRAID, like
everything else.** This item originally read "plain-LE is CORRECT for the M-5000
(OHRCA) but WRONG for M-200/M-300", i.e. a per-generation switch. That split is
dead: the zoneA/zoneB goldens are the M-5000's OWN two REAC ports carrying program
audio, and they decode at coherence 0.988/0.981 under the braid versus 0.234/0.236
under plain-LE (the table in [`VALIDATION-PLAN.md`](VALIDATION-PLAN.md) Stage B).
The "plain-LE is rig-validated on a live M-5000, coherence 0.999" evidence that
produced the split is explained there as a mid→hi lane shift amplifying quiet
braided audio 256× into a coherent-looking image — wrong-layout decodes can look
BETTER than the truth on quiet material. The operator has since confirmed that
every mixer generation puts out the same downstream format, so per-generation
divergence is not an open question, it is a discarded guess.

The braid is the wire format in both directions (libreac's `<reac/reac_braid.h>`
is the oracle, and `src/reac_tx.c` has encoded with it unconditionally since the
`REAC_TX_LAYOUT` override was removed in a4f7359). The RX side followed on
2026-07-29 by way of the dependency rather than the call site: **libreac 0.5.0**
un-braids inside `reac_decode()` with an unchanged signature, so raising the
floor to `>=0.5.0` made `src/reac_rx.c` correct with no logic change. Independent
verification upstream: on a frame libreac itself built, 0 of 480 samples agreed
before, 480 of 480 after. The plain-LE layout survives only as the explicitly
named diagnostic `reac_decode_plain_le()`, which reac-pw does not call.

Wrong layout = plausible-sounding noise, so the on-wire re-check still wants LOUD
program, never a quiet room.

Files: `src/reac_rx.c` (`gate_accepts`, `feed_frame`).

### W5 — On-wire validation against a real master  ·  **PASSED on V-Mixer, OPEN on OHRCA**
`test_reac_courtship.c` only proves our-slave ↔ our-master, so a shared wrong
assumption passes it. The wire test: a real desk GRANTs our cold-connect, reaches
ESTABLISHED, streams to us, and shows our upstream on its input meters.

Result: a real cold-booted **M-200** grants and enrols reac-pw, and holds 300 s —
"W5 CONNECTED" below, with the census and the config-announce frame that unlocked
it. The **M-5000** grants us and then reverts to hunting; still open. The input-meter
half is blocked on W1 (nothing fills `tx_ring` in the slave role yet).

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
- ~~**W4 layout** — plausible-noise failure mode while RX decodes plain-LE and TX
  encodes the braid (#80).~~ **Closed 2026-07-29** with the libreac `>=0.5.0`
  floor: both directions read the one braid. A wrong layout still fails quietly
  rather than loudly, so the on-wire re-check wants loud program.
- **W5 on OHRCA** — the M-5000 grants our slave and then re-hunts. Settled on
  V-Mixer (M-200 enrols us and holds).
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
real desk accepts it as a settled box. The M-5000 (OHRCA) is still open, and the
cause is the **OHRCA established-state shape** that reac-pw does not yet emit, NOT
a hardware crystal requirement.

⚠ **This paragraph originally named that shape as "1494 B frame + per-frame CRC-16
trailer, 96 kHz upstream" and set the remaining work as "emit the OHRCA-width
upstream + CRC-16 trailer". Both halves of that are dead:**

- The **1494 B downstream frame**: read as a mirror/SPAN artifact — 2 bytes of the
  Ethernet FCS — the very next day, and reproduced independently for the master
  direction (W4(a) below; `docs/MASTER-HARDWARE-VERIFY.md`, "The 1494-byte frame
  is a capture artifact"). That reading is contested by libreac's `<reac/reac.h>`
  and is being re-checked (#80; see the ⚠ note under W4(a)). Either way there is
  nothing here for a slave to emit: on the artifact reading a literal 1494 B
  payload puts 2 garbage bytes ahead of the NIC's own real FCS, and on the
  trailer reading the bytes are the master's to produce, not the box's.
- The box **upstream** `+2` IS real (a genuine OHRCA CRC-16 trailer on the S-4000's
  1206 B returns), but it is an **RX-strip** concern, not something a slave emits —
  see [`OHRCA-UPSTREAM-DUPLICATE-FRAMES.md`](OHRCA-UPSTREAM-DUPLICATE-FRAMES.md)
  and the `UP32A`/`UP32B` fixtures in `tests/upstream_fixtures.inc`.

The leading remaining suspect for the M-5000 gap is the 96 kHz upstream cadence.
The other half of that pair — a per-generation downstream audio layout
(W4(b)/#135) — is **gone**: there is one downstream format across every mixer
generation, and since 2026-07-29 both directions decode it (see W4(b) and #80).
Do not budget work for a downstream trailer either; whichever way the `+2` reading
lands, a slave never emits one.

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
