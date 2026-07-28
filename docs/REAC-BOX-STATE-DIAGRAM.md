# REAC stagebox establishment — state diagram (capture-verified)

> **✅ VALIDATED ON THE WIRE (M-200, 2026-07-11).** reac-pw ran as a 16-ch slave
> against a real, cold-booted **M-200** (V-Mixer, 48 kHz) and the desk **enrolled
> it as a stagebox in its REAC menu** and held the connection: master `GRANT`
> burst → `PROBE 0/s` for the full run → steady `3999` fill/s both directions,
> CHANMAP + heartbeat ~0.5/s. Proof: `reacpw-slave-m200-CONNECTED-2026-07-11.pcap`.
> **This falsifies the earlier "hardware / clock-domain wall" conclusion** — reac-pw
> paces from `CLOCK_MONOTONIC`, and a real desk still accepts it as a settled box.
> The remaining M-5000 (OHRCA) gap is NOT a clock wall. ⚠ It is also NOT a
> "+2 CRC-16 trailer" — that theory was FALSIFIED 2026-07-12: the 2 extra bytes
> some captures show are the **Ethernet FCS** (`CRC-32(frame)[:2]`), a mirror/SPAN
> artifact, not a REAC field (proof below). The real OHRCA gap is still open —
> most likely the per-generation downstream audio layout (W4/#135) — but there is
> nothing to crack or emit for a trailer. See the M-200 section below.
>
> **UPDATE 2026-07-25 — the FCS finding is DOWNSTREAM-only.** The falsification
> above stands for the master's DOWNSTREAM frames (1492 → "1494" = 2 bytes of
> Ethernet FCS from a mirror/SPAN config). The box's UPSTREAM is different: a
> real S-4000's returns are **1206 B = 1204 + a REAL OHRCA CRC-16 trailer**
> appended after the `c2 ea` end marker — a genuine box field, proven on
> non-mirror-artifact captures (in one capture our own downstream frames are
> all 1492 B with no +2 while the box's upstream are 1206 B; the box also mixes
> ~1/8 trailerless 1204 B frames, which an FCS could never do). reac-pw strips
> it on RX (`src/reac_upstream.c`). Evidence:
> [`OHRCA-UPSTREAM-DUPLICATE-FRAMES.md`](OHRCA-UPSTREAM-DUPLICATE-FRAMES.md)
> + the checked-in real 1206 B frames `UP32A`/`UP32B`
> (`tests/upstream_fixtures.inc`, from `matrix-m200-s4000-2026-07-24.pcap`).
> So: downstream +2 = FCS artifact (do not emit); upstream +2 = real trailer
> (strip on RX, `%36` width check after).

Reconstructed from live M-5000 (OHRCA, 96 kHz) captures, 2026-07-11: a real
S-1608 cold-boot (`real-s1608-coldboot-m5000-2026-07-11.pcap`) and reac-pw's
slave sessions, both viewed on the M-5000's port mirror so the master's response
is in the same frame of reference. Frame labels are keyed by L2 source (box
`00:40:ab:c4:80:3b` vs master `00:40:ab:ca:15:4c`) — critical, because BOTH sides
use `cdea 04 03` (box = cold-connect JOIN, master = GRANT).

## State machines — the ARROWS are packets (TX = we send, RX = from master)

### BOX (what reac-pw must be)

```
 PHY_DOWN
    │  ◇ event: PHY up
    ▼
 FLOOD
    │  ↑TX  broadcast FILLER (type 0000) ×~5460   (bounded burst)
    │  ↓RX  any master frame → learn master MAC
    │  ══ transition: flood count reached AND master learned ══▶
    ▼
 COLD_CONNECT
    │  ↑TX  CONFIG-ANNOUNCE  cdea 01 03 0010 sel 82   ← SETUP DECLARATION
    │  ↑TX  JOIN escalation  cdea 04 03  0014→0013→0016→001a  (~100ms grid)
    │  ↑TX  early HEARTBEAT  cdea 01 03 0001
    │  ↓RX  master PROBE / CHANMAP / CFEA   (no transition)
    │  ══ transition ON ↓RX  master GRANT  cdea 04 03 0013 ══▶
    ▼
 TX_MUTE  (settle)
    │  ══ transition: dwell elapsed ══▶
    ▼
 ESTABLISHED
    │  ↑TX  FILL 8000/s  +  HEARTBEAT cdea 01 03 0001 ~1/s
    │  ↓RX  master CHANMAP  (re-arm link-check)
    │  ══ transition ON ↓RX peer-gone / master-MAC-change ══▶ re-FLOOD
```

### MASTER (the M-5000 — the state we must DRIVE it into)

```
 HUNTING                                   LOCKED  (goal)
   ↑TX PROBE 0100001a (~high)                ↑TX CHANMAP ~2/s, PROBE=0
   ↑TX CHANMAP, CFEA, SUB01/02               (no probe, no re-grant)
      │                                         ▲
      │ ↓RX box JOIN (cdea 04 03)               │ ↓RX box ESTABLISHED
      ▼                                         │    (fill + HB) ACCEPTED
 GRANTING ───────────────────────────────────────┘
   ↑TX GRANT cdea 04 03 0013 (burst ~100)
   ↑TX CHANMAP ~2/s, PROBE=0
      │
      │ ✗ reac-pw: GRANTING ──▶ back to HUNTING   (master rejects our ESTABLISHED)
      ▼
 (re-HUNTING: PROBE ~680/s, CHANMAP→0)
```

**This crux is OHRCA-specific (M-5000), NOT universal.** On the **M-200
(V-Mixer)** reac-pw drives the master cleanly into LOCKED and the desk enrolls it
(PROBE 0/s, box shown in the REAC menu) — the `GRANTING → LOCKED` edge is
satisfied. On the **M-5000 (OHRCA)** the master takes `GRANTING → HUNTING`
instead (PROBE ~680/s post-grant). Since the same reac-pw build, same pacer, same
V-Mixer-shaped upstream locks the M-200 but not the M-5000, the missing arrow is
an **OHRCA-shaped ESTABLISHED stream** (96 kHz upstream; likely the per-generation
audio layout — NOT a DOWNSTREAM CRC trailer, that was the Ethernet-FCS artifact;
the box's UPSTREAM +2 trailer is real but is an RX-strip concern, not something
to emit — see the falsified-trailer note + its 2026-07-25 update) — not a
clock/hardware property of the box. reac-pw's *own*
transitions are right
on both desks (it floods, cold-connects, is granted, establishes); only the
OHRCA post-grant emission is still unmatched.

## Capture evidence (per-second, master-port mirror)

Real S-1608 → M-5000:
```
 t │ BOX: FLOOD JOIN CFG HB fill │ MASTER: PROBE GRANT CHANMAP
 0 │ 10918    6   1   1  2533   │     0     0     2     ← FLOOD (+config/join/hb tail)
 1 │     0    2   0   1  7997   │     0   112   2       ← COLD_CONNECT, master GRANTS
 2+│     0    0   0   1  7999   │     0     0     2     ← ESTABLISHED, master CALM
```

reac-pw → M-5000 (commit 57117c7):
```
 t │ BOX: FLOOD JOIN CFG HB fill │ MASTER: PROBE GRANT CHANMAP
 0 │ 10902    4   0   0  2575   │     0     0     2     ← FLOOD (no config/hb yet)
 1 │     0    1   1   1  4045   │     0    52   2       ← COLD_CONNECT+config, master GRANTS
 2+│     0    0   0   1  7999   │   680     0     0     ← master REVERTS TO PROBE ✗
```

## reac-pw FSM mapping (`src/reac_fsm.c`)

| protocol state | reac-pw FSM state | status |
| --- | --- | --- |
| FLOOD | `FSM_FLOOD_ANNOUNCE` | ✓ ~5460-frame bounded flood |
| COLD_CONNECT | `FSM_COLDCONNECT` | ✓ escalation + config + hb (6-phase cycle) |
| (grant→settle) | `FSM_TX_MUTE` | ✓ dwell, on GRANT rx |
| ESTABLISHED | `FSM_ESTABLISHED` | ✓ fill + hb |

reac-pw reproduces every phase, emits the byte-identical frame set, and **is
granted**. Frame content is not the gap.

## The one open divergence

After the grant, the real box's master stays calm (PROBE=0); reac-pw's master
**reverts to hunting** (PROBE ~680/s, CHANMAP→0) — it enrolls us but does not
treat us as a *settled* box. Leading suspect (capture-visible): **config-announce
timing** — the real box declares its setup at **t0 (inside the flood, before the
grant)**; reac-pw sends it at **t1 (phase-4 of the cold-connect cycle, at the
grant)**, so the master may grant before it has our setup. Candidate fix: emit
config-announce at the START of establishment (flood tail / first cold-connect
slot), not mid-cycle. Unverified — next experiment.

## Cross-mixer validation — M-200 cold boot (fresh, 2026-07-11)

Re-captured a real S-1608 cold boot on an **M-200** (48 kHz) — a different mixer —
and it matches the diagram edge-for-edge (dedup'd; `real-m200-s1608-coldboot-...pcap`):

```
 t │ BOX: FLOOD JOIN CFG HB fill │ MASTER: PROBE GRANT CHANMAP
 0 │  4000    -   -  -    -     │    0     -     1     FLOOD
 1 │  1459    6   1  2  2532    │    0     -     1     COLD_CONNECT (config+join+hb)
 3 │    -     2   -  -  3998    │    0    56     -     master GRANTS
 4+│    -     -   -  1  3999    │    0     -     1     ESTABLISHED, master PROBE=0 (LOCKED)
```

Confirms: (1) same state machine on a different desk; (2) the rate law — fill =
4000/s (48 kHz) here vs 8000/s (96 kHz) on the M-5000, `pps = rate/12`; (3) the
LOCKED reference — a real box drives the master to **PROBE = 0**.

## Box IDENTITY vs CHANNEL COUNT — three independent axes (2026-07-11)

Live M-200/M-200i testing separated what had been conflated into one
`--box-channels` knob. A box on the wire is described by **three orthogonal
fields**, and the desk uses them differently:

1. **Selector byte** (`cdea 01 03 0010`, frame `[22]`). Mixer-independent
   (same on M-200/M-200i/M-300/M-5000). VERIFIED so far:
   - `0x82` → the desk names it **S-1608** (and sends no ASCII name frame).
   - `0x84` → the selector the real **S-0808** uses (see field 2). A `0x84`
     announce with **no** name frame makes the M-200 *fall back* to labelling it
     **"S-4000S"**. ⚠ That fallback label is the DESK'S GUESS for a nameless
     `0x84` box — it is NOT proof that a real S-4000 uses `0x84`, nor that S-4000
     and S-0808 share a selector/family. **UNVERIFIED**: the real S-4000's
     selector + announce are unknown until an S-4000 is captured. Do not assume.
2. **Exact model — an ASCII name frame** (`cdea 04 01 001b`). The S-0808 spells
   `53 2d 30 38 30 38` = **"S-0808"** in this frame (box `c4:dc:9c`, VERIFIED).
   When reac-pw emitted a `0x84` config-announce **without** this frame, the M-200
   fell back and showed **"S-4000S 8 in / 8 out"** — a config that doesn't exist
   (the real S-4000S is **32 in / 8 out**), i.e. a nameless-`0x84` guess, not a
   real box. So for a `0x84` box the displayed model comes from this ASCII string.
   (V-Mixer `0x82` boxes are named by selector and send no ASCII name.)
3. **Channel map + width — the descriptor list + frame length.** The
   config-announce descriptor (`02/01/03` run after the selector) varies with the
   box's I/O even at the same selector (two different `0x84` boxes carry different
   descriptors). The on-wire audio width is `box_frame_len(in_ch) = 52 + 36·in_ch`
   (S-0808 8-in → 340 B, S-1608 16-in → 628 B; config-announce runs +2 on a real
   box — 342/630 — but reac-pw's width-exact 340/628 is accepted). This is why the
   **establishment is channel-count-parameterized**: every fill/upstream/announce
   length scales with `in_ch`, while the phase sequence itself is identical.

4. **⚠ Cold-connect INVENTORY frames — the actual model discriminator (VERIFIED,
   2026-07-11).** The selector + name are NOT sufficient. The mixer identifies the
   model from the full cold-connect frame set: the escalation blocks
   `04 03 0016` and `04 03 001a` carry MODEL-SPECIFIC inventory (the S-0808's
   differ byte-for-byte from the S-1608's), and the S-0808 also sends an extra
   `04 02 000d` frame the S-1608 never does. Proof: reac-pw emitting an S-0808
   config-announce + "S-0808" name **but the S-1608's `0016`/`001a` and no
   `0402000d`** still displayed as the generic **"S-4000S 8 in / 8 out"**. Only
   after reac-pw sent the S-0808's real `0016`/`001a` + `0402000d` did the M-200
   show **"S-0808"** (live-verified). So a faithful emulation must replay the
   COMPLETE per-model frame set, not just selector + name. Frames that are
   model-generic (verified identical S-1608 vs S-0808): `0014`, `0013`, heartbeat
   `01030001`. Frames that discriminate: `01030010` (selector), `0401001b` (name),
   `04030016`, `0403001a`, `0402000d`.

**Firmware / REAC version.** The M-200i displays the S-0808 as **firmware 1.003,
REAC 1.000**. These are read by the desk from the box, confirming a version field
exists on the wire — but it is NOT isolable from our establishment captures (the
`010000` bytes inside cold-connect blocks are not a clean match, and the constant
`12 12` in every cold-connect block is family-wide, not per-unit). Most likely the
version is returned in a **device-info poll** that fires when the operator opens
the box's detail page, which our establish/steady captures don't include. OPEN:
capture while opening the box info page, ideally two boxes of differing firmware
to diff. Do NOT guess the encoding.

### The FIXED model matrix (byte-verified rows only)

Models are fixed rows — a model determines its selector, name frame, descriptor,
and in/out. There is no "S-1608 with 8 channels": pick a row.

All three rows below are LIVE-VERIFIED on a real M-200 (2026-07-12): reac-pw
`--box-model {s1608,s0808,s4000s}` enrolled and the desk displayed each correctly.

| model | selector | name frame | 0402000d | in / out | audio width | desk shows |
| --- | --- | --- | --- | --- | --- | --- |
| S-1608 | `0x82` | (none — named by selector) | no | 16 / 8 | 628 B | **S-1608** ✅ |
| S-0808 | `0x84` | `04 01 001b` "S-0808" | yes | 8 / 8 | 340 B | **S-0808** ✅ |
| S-4000S | `0x84` | (none — see below) | no | 32 / 8 | 1204 B | **S-4000S** ✅ |

**The `0x84` family default IS "S-4000S" (VERIFIED 2026-07-12).** The real S-4000S
(`s4000s-coldboot-m5000-...`, box `c4:06:80`) sends selector `0x84` with **no name
frame and no `0402000d`** — so the desk's default label for a nameless `0x84` box
is genuinely "S-4000S". The **S-0808** is the exception: it adds the `0401001b`
name frame to override the default. Each model's `04030016`/`04030001a` inventory
differs (S-1608 `02 02`, S-0808 `01 00`, S-4000S `02 05` at the discriminating
byte). The S-4000S heartbeat also carries channel-slot data (`29 38 00 …`) where
S-1608/S-0808 send an all-zero heartbeat — reac-pw sends the generic heartbeat and
the M-200 still enrolled it, so the heartbeat is not identity-bearing. Note: the
S-4000S's real frame is 1204 B (`box_frame_len(32)`), ending in `c2ea`; reac-pw
emits exactly that and is accepted. ~~(Some M-5000 captures showed 1206 B — that
was 2 bytes of the Ethernet FCS from a mirror config, NOT a box field.)~~
**CORRECTED 2026-07-25:** the 1206 B upstream frames carry a REAL OHRCA CRC-16
trailer after the end marker (`1206 = 52 + 32·36 + 2`), interleaved with ~1/8
trailerless 1204 B frames — a box field, not the FCS (that artifact remains true
only for the master's DOWNSTREAM 1492→1494 case). reac-pw strips the trailer on
RX; see [`OHRCA-UPSTREAM-DUPLICATE-FRAMES.md`](OHRCA-UPSTREAM-DUPLICATE-FRAMES.md)
and the captured `UP32A`/`UP32B` fixtures (`tests/upstream_fixtures.inc`).

The S-4000 merge/split units (`c4:06:80`, `c4:08:bc`) are also `0x84` with a
distinct descriptor; their menu names are unconfirmed → not yet rows.

### Channel-count parameterization of the state machine

The phase graph (PHY_DOWN → FLOOD → COLD_CONNECT → [GRANT] → TX_MUTE →
ESTABLISHED) is **identical for every model**. Only these quantities are functions
of `in_ch`:

| quantity | formula | S-0808 (8) | S-1608 (16) |
| --- | --- | --- | --- |
| upstream / fill width | `52 + 36·in_ch` | 340 B | 628 B |
| config-announce len (real box) | width + 2 | 342 B | 630 B |
| fill rate (pps) | `rate / 12` | 4000 @48k | 4000 @48k |
| flood burst count | ~5460 (rate-independent) | ~5460 | ~5460 |

### Warm-relink (hot boot) — OPEN

The cold-boot path above is fully captured. The **warm relink** (box already known
to the desk) is documented to skip FLOOD, but we lack a clean capture of the
*transition* (`m200-s1608-BIDIR-reboot` starts already-established). Capturing it
needs a bidirectional mirror (to see the box's egress) + a hot boot.

## reac-pw ON the M-200 — enrolled (2026-07-11, `reacpw-slave-m200-CONNECTED-...pcap`)

Same rig, reac-pw as a 16-ch slave (`--src-mac 00:40:ab:c4:80:41`) into the
freshly-booted M-200. Matches the real-box cold-boot census edge-for-edge:

```
 t │ BOX: fill JOIN CFG HB │ MASTER: PROBE GRANT CHANMAP   note
 2 │ 4000    0   0   0     │   257     0     0             master HUNTING (probing for a box)
 4 │ 3221    3   1   1     │     0     0     1             COLD_CONNECT: config-announce + join + hb
 5 │ 3582    2   1   1     │     0    38     0             master GRANTS
 6 │ 3615    0   0   0     │     0    18     1             grant tail
 7+│ 3999    0   0  ~.5/s  │     0     0    ~.5/s          ESTABLISHED — PROBE 0/s, box in REAC menu
```

The config-announce (`cdea 01 03 0010`, the setup declaration) at the cold-connect
step is what the M-200 registers into its REAC-device inventory; the desk shows
the box connected. Stable across a 300 s run (no re-hunt, no drop).

## The state machine is MASTER-INDEPENDENT (verified)

Confirmed against M-200i, M-300, and M-5000 captures (and S-0808 / S-1608 /
S-4000S). Per the mixer-vs-box matrix (`reac-firmware-re/MIXER-VS-BOX-MATRIX.md`)
the master contributes only its **MAC** + a one-byte **generation flag**
(cfea `+17`: `0x00` V-Mixer / `0x01` OHRCA) — the protocol and this state diagram
are **identical** regardless of desk. So the box behaviour that drives
`GRANTING → LOCKED` is the same to emulate for any mixer.

## Cross-capture baseline: probe rate is a GRADED lock signal, not binary

A master keeps a low background probe even when fully locked to a real box, and
reac-pw drives it much higher — the un-pinned `→ LOCKED` gap (all dedup'd,
same-day M-5000/M-200 captures):

| master + box | master PROBE rate |
| --- | --- |
| M-200 + real S-1608 (established) | ~14 /s |
| M-5000 + real S-1608/S-0808/S-4000S (established) | ~11–91 /s |
| **M-5000 + reac-pw (granted, streaming)** | **~680 /s** |

reac-pw is granted and streams, but the master keeps hunting ~10× harder than
with a real box — so we satisfy every transition UP TO the grant but not the
established-state behaviour that quiets the master. Ruled out for this gap:
frame content (byte-identical), config-announce timing, and a separate socket
(the box↔mixer link is 100% 0x8819 — no ARP/IP/other ethertype).

## Superseded readings (do not re-introduce)

- "clock domain / not software" (round 1) — an artifact of an intersection-only
  diff that hid the entirely-missing config-announce; adding it produced the grant.
- "hardware / clock-domain wall — needs a real word clock (#131)" (round 2, from
  the M-5000 post-grant re-hunt) — **falsified 2026-07-11**: the same reac-pw
  build, `CLOCK_MONOTONIC`-paced, enrolls in a real M-200 (in the REAC menu,
  PROBE 0/s, stable). The M-5000 gap is OHRCA established-state shape, not a clock.
- "master GRANT=0 for reac-pw" — an analysis script that labeled all `04 03` as
  cold-connect hid the master's grants. Always key `04 03` by L2 source (box JOIN
  vs master GRANT).
