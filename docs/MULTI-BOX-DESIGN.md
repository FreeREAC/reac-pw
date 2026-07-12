# Multi-box master — design

Goal: reac-pw as a Roland desk driving **several stageboxes at once** on one REAC
segment, each box's inputs landing at its own slice of the 40-slot fabric, exposed as
`reac-capture` channels (and, later, each box's outputs fed from `reac-playback`).
Optional per-box **names**. See also `MASTER-HARDWARE-VERIFY.md` (single-box, verified).

## What is single-box today

- `reac_master` FSM tracks ONE box (`box_mac`); a JOIN from a different MAC is
  `DROP_MAC_CHANGE` — it drops the current box and re-grants the new one.
- `reac_rx` locks `up_src` to the FIRST box and decodes its `nch` channels into ring
  slots `0..nch-1`; frames from any other box are gated out (`frames_other`).
- One `reac_ring` (40 ch), one shared write cursor; `reac_source_node` reads it into
  the 40 `reac-capture` ports.

## Target architecture

### Box registry (shared)
`struct reac_box { uint8_t mac[6]; int base; int nch; char name[...]; established; }`,
a small fixed array (`REAC_MAX_BOXES`, say 5 — 5×8 = 40 slots min). Owned where both
the FSM and RX can see it (master + a parallel RX table, or a shared registry). A box
is **allocated a contiguous fabric base** on first JOIN: `base = Σ widths of earlier
boxes` (S-1608=16 → base 0; a following S-0808=8 → base 16; …), capped at 40.

### RX — multi-source (the upstream / mic direction; MOST valuable, master-side)
- `gate_accepts`: keep a MAC→box table instead of one `up_src`. Known box → accept and
  tag with its `base`/`nch`. Unknown box-shaped src → register a new box (allocate
  base) if room, else drop.
- **Ring-merge problem (the crux):** `reac_ring_write` writes ALL 40 channels each
  call, so box B's frame (its slots filled, the rest zero) would clobber box A's slots.
  Options: **(a)** one ring PER box (width `nch`), the source node reads N rings and
  places each at its `base` — clean isolation, N cursors, chosen design; **(b)** a
  partial-channel ring write (`reac_ring_write_slots(base, nch, …)`) that touches only
  a box's rows. (a) is simpler to reason about and keeps each box's counter/ppm
  independent (they free-run separately). Go with **(a)**.
- `reac_source_node`: read each box's ring into `dst[base .. base+nch)`; zero the
  unallocated slots. Per-box ppm/rate-match (each box is its own clock domain — but on
  one segment they share the master's clock, so one rate loop is fine to start).

### Master FSM — per-box establishment
- Generalise IDLE/PROBING/GRANTING/ESTABLISHED to a **per-box** sub-state while the
  global cadence (probe cycle + chanmap sweep, already fabric-wide) keeps inviting new
  boxes. A JOIN from an UNKNOWN MAC with room → grant that box (its model's burst) and
  register it; JOIN from a KNOWN established box → hold (re-arm its link-check), do NOT
  drop the others. Each box gets its own `link_check` peer-gone budget + heartbeat.
- **UNKNOWN wire detail (needs a 2-box capture or the live S-0808):** how a real desk
  assigns each box its DISTINCT fabric slots — is it purely master-side (we place the
  box's channels wherever we want and the box is agnostic upstream), or does the desk
  tell the box its base via the grant/chanmap (matters for the DOWNSTREAM / box
  outputs)? The upstream/mic direction is master-side and does NOT need this; the
  downstream/box-output direction DOES. So: land upstream multi-box first (verifiable
  with mics), defer downstream multi-box until a 2-box capture RE's the slot field.

### Names (optional) — independent, ship first
`--box <model>[:name][@base]` (repeatable) OR `--box-name <mac>=<name>`. Store on the
box registry; publish per box into the PipeWire node/port metadata so openmixer shows
"Drums (S-1608) ch 1-16" rather than bare `capture_01`. With no name, fall back to the
recognised model + MAC tail. Deterministic base assignment (join order) unless `@base`
pins it. This piece needs NO second box and NO wire RE — testable on the S-1608.

## Build order
1. **Names + box registry scaffold** (single box) — testable now, requested.
2. **RX box-table + per-box ring** (still 1 active box; unit-test 2 synthetic boxes) —
   structural, single-box behaviour preserved.
3. **Master per-box grant/keep-alive** (unit-tested; live-validate when the S-0808 is
   powered + linked — it is silent on the wire as of 2026-07-12, needs to be on).
4. **Downstream per-box outputs** — gated on a real 2-box capture to RE slot assignment.

## Status
Design only for 3–4; pieces 1–2 are the safe, testable start. The S-0808 must be
transmitting (it is not currently) to validate 3; a real desk + 2-box capture unblocks 4.
