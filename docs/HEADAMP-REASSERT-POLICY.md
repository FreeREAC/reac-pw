# The head-amp re-assert policy

**Status: implemented and SHIPPED DISABLED.** The mechanism is built and unit-
tested (`reac_headamp_tx`, the ESTABLISHED FILLER overlay in `reac_pacer`), and
`REAC_HEADAMP_RESWEEP_SECONDS` is **0**, which turns it off. Any positive value
enables it.

It stays off pending two things, in this order: **the operator's acceptance**,
because a refresh overrides a change made at the box's own front panel within one
cadence and taking that authority over 48 V is a decision about who owns the
stage, not a scheduling detail; and **the capture gate** at the end of this file,
because no capture in this repo has ever shown a box receiving a redundant
head-amp record — no master here ever sent one — so "a rewrite is harmless" is a
deduction and not a measurement.

**Who holds the job meanwhile.** openmixer's scene watch: it re-applies its
recorded scene after a settle window (`server.ts`, `REAC_SCENE_SETTLE_REPLAY_MS`),
which is the console-side answer to the one refresh case that IS measured — a
freshly booted box ignores head-amp records while its input board initialises.
That watch is the standing writer, it writes through the same door
(`reac.headamp.<ch>.<param>` on this node), and enabling the cadence here does not
replace it: the two would then both be asserting, which is another reason the
switch is the operator's.

## The divergence this settles

Two places in this repo describe the master's head-amp table as something it
**re-asserts**:

- `DESIGN.md`'s file table: "the declarative/DMX table the master re-asserts
  (full table on a slow period + an edge record on change)";
- `reac_sink_node.c`: "write-through control re-asserted on the wire by the DMX
  scheduler".

The code did not do it. `reac_headamp_tx_next` emitted dirty cells and a one-shot
scene replay armed at establishment, and once both drained it returned 0 forever.
Measured on the live rig (2026-09-06/07, openmixer
`docs/design/notes/2026-09-06-rig-headamp-and-clip-findings.md` §3): 400 000
frames spanning a phantom write and the following 30 s carried exactly **three**
head-amp records — phantom, pad, sens for the one channel written — and then
nothing at all.

So the wire agreed with the code and both disagreed with the design. This note
picks the design's side, narrowed, and the code now implements what is written
here.

## Why the old ruling said silence, and why it is not enough

The silence was not arbitrary. Committed captures of a real M-200 driving an
S-1608 hold 20.8 s / 27.8 s / 33.0 s of established traffic with phantom lit and
**zero** head-amp records (`COLDCONNECT-clean-2026-07-24`,
`BIDIR-reboot-2026-07-11`, `matrix-m200-s1608-2026-07-11`); the 1 Hz frame is the
op-0103 CHANMAP heartbeat and carries no head-amp cell. A box HOLDS its committed
state while it is powered, and a box that power-cycles re-courts, so the
establishment scene replay restores it.

That reading is right about the M-200 and about a powered box. It is a thin
guarantee for a show, for two reasons the captures cannot speak to:

- **Silence is only correct while the box's state and ours provably agree, and
  the protocol has no readback.** There is no way to ASK a box what its phantom
  pins are doing. A master that asserts once has no mechanism that could ever
  discover a disagreement, let alone correct one — and the operator's interface
  keeps showing our copy.
- **One lost frame is one lost setting, permanently.** A head-amp record rides a
  single FILLER slot, unacknowledged. A frame the NIC drops, a slot the pacer
  abandons under debt, a record the box discards because its input board is still
  initialising after a warm reset — each of those leaves the desk and the box
  disagreeing until the next establishment, which on a healthy link may be hours.

"A real desk does not do this" is an argument about the M-200, not about
correctness. The captures license silence; they do not require it.

## The policy

**Once ESTABLISHED, after a period of head-amp silence on the wire, the master
re-emits the cells the operator has SET — and only those.**

Concretely, in `reac_headamp_tx`:

1. The re-assert is a **re-arm of the existing replay**, over the same box slots
   the establishment scene replay uses, in **set-only** mode. There is no second
   scheduler and no new state: one cursor, one stride, two modes.
2. **Set-only, always.** A cell the operator never set carries the *enrolling
   default* (`phantom OFF`, `pad OFF`, `SENS 0x20`) in the establishment scene,
   because a channel armed all-zero never enrols. Those defaults are correct at
   ARMING and wrong as a repeated assertion: re-sending them every couple of
   seconds would drive phantom OFF on a channel somebody lit from the box's own
   panel, and would overwrite a sensitivity we were never asked to own. Absence
   of a cell means we have no opinion, and no opinion is silence.
3. **The cadence is measured from the last head-amp record actually emitted**,
   not from a free-running clock. An operator edge, or an establishment scene
   replay, resets it. Two sweeps can therefore never overlap on the wire, and a
   busy console — one that is already writing — never has re-assert traffic
   stacked behind its edges.
4. **It is off unless armed.** `reac_headamp_tx_init` leaves the period at 0, so
   the pure module is silent by default and every existing pin on "no records
   after the scene drains" still holds. The pacer arms it, in frames, from the
   wire rate — which is also why the re-arm survives a rate change: the period is
   re-derived at the new fps by `reac_pacer_apply_rate`.
5. **`REACPW_NO_HEADAMP` still silences everything.** The re-assert is gated on a
   scene having been armed, and that knob is exactly what suppresses the arming.

### The cadence, and what it costs

`REAC_HEADAMP_RESWEEP_SECONDS` is **0 — disabled**, and the intended value once
it is accepted is **2 s**. It is a POLICY, stated as a named constant with that
word in its comment: an operator who wants a slower refresh on a congested
segment, or a faster one for a rig that is losing records, changes this and
nothing else.

The cost, at the intended 2 s. **The two ratios below have different
denominators and this note originally chained them into one sentence**, which
read as though 72 ms of every 2 s were 0.3 % — it is not; 72 ms of 2 s is 3.6 %
of elapsed time, and 0.3 % was the share of SLOTS. Stated separately, for a
32-input box with all 40 head-amp cells set, at 96 k (8000 fps):

| quantity | value |
|---|---|
| records per sweep | 40 |
| sweep length | (40−1) × 12 + 1 = **469 frames = 59 ms** |
| cycle | 2 s of silence + the sweep = **≈ 2.06 s** |
| average record rate | **19.4 op-0403/s** |
| elapsed time spent sweeping | 59 ms / 2.06 s = **2.9 %** |
| slots carrying a record | 40 / 16 469 = **0.24 %** |
| frames ADDED to the wire | **zero** |

The last row is the one that matters: records ride `REAC_HEADAMP_SWEEP_STRIDE`
(12) frames apart on FILLER slots the pacer emits anyway, overwriting a control
block that would otherwise be idle. No audio moves, no probe/grant/chanmap/cfea
frame is ever displaced (the pacer's overlay guard is unchanged), and the frame
count on the wire is identical with the refresh on and off. A desk with nothing
set emits nothing at all.

### What this is NOT

It is not a state, and it is not a timer invented at a call site — `MASTER-FSM.md`
carries the law that forbids both. It is an ESTABLISHED **overlay**, beside the
operator edge and the entry-armed scene replay that were already there, and it is
recorded in that file with them.

## The risk, named

A repeated absolute `op-0403` write is idempotent *by the protocol audit*: a lone
op-0403 self-commits and the LED follows, with no commit pair on the wire
(`HEADAMP-PROTOCOL-AUDIT-2026-07-22`). Re-sending the value a channel is already
holding should therefore be a no-op at the box.

**That is a deduction, not a measurement.** No capture in this repo shows a box
receiving a redundant head-amp record, because no master in this repo ever sent
one. That is why the shipped constant is 0.

**And a second cost, which is not about the box at all.** A refresh asserts the
console's copy over anything else that changed the cell — including a hand on the
box's own front panel. Today an engineer at the stage can lift a pad or drop
phantom and the desk simply goes out of step; with the cadence on, the desk wins
back within one period and the engineer's change disappears with no message.
Which of those is right is a decision about who owns the stage, and it is the
operator's, not this file's.

If a box turns out to glitch its preamp on a rewrite, or the panel-override
behaviour is unwanted, the mitigation is the constant, not a redesign: set it to
0 and the behaviour is exactly the assert-once one a real M-200 shows.

## The rig gate

Unit tests pin the scheduler (`tests/test_reac_headamp_tx.c`: after the edge and
the scene drain, a re-arm emits the set cells again, in order, and never an unset
cell). They cannot show the box is happy. Before this is trusted on a show:

1. With one channel's phantom ON and the rest unset, capture ≥ 30 s of
   established wire (`cdea` marker at **offset 16** — not 30; three captures were
   read as "zero head-amp frames" for exactly that mistake) and require: the set
   cells appear once per `REAC_HEADAMP_RESWEEP_SECONDS`, and no record ever
   carries a channel/param the operator did not set.
2. Watch the box's 48V LED across ≥ 10 re-assert cycles: it must not flicker,
   drop, or re-arm audibly.
3. Prove the refresh does the job it exists for: with the link established, clear
   the box's pin by power-cycling ONLY the box's preamp state (or by writing the
   cell from another master), and require the LED to come back within one
   cadence — without a re-establishment.

Until step 1 has been run against a real box AND the operator has accepted the
panel-override behaviour above, `REAC_HEADAMP_RESWEEP_SECONDS` stays 0 and this
Status line says so. Turning it on is a one-constant change with a unit test that
already pins the default (`tests/test_reac_headamp_tx.c` asserts the shipped 0),
so the flip cannot happen silently.
