# REAC stagebox establishment — state diagram (capture-verified)

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

**This is the crux.** For the real box the master takes `GRANTING → LOCKED` on
receiving the box's ESTABLISHED stream. For reac-pw it takes `GRANTING → HUNTING`
— so the missing arrow is a **box packet that drives the master into LOCKED**
which reac-pw isn't sending (correctly). reac-pw's *own* transitions are right
(it floods, cold-connects, is granted, establishes); the failure is that our
post-grant emission does not satisfy the master's `→ LOCKED` edge.

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

- "clock domain / not software" — an artifact of an intersection-only diff that
  hid the entirely-missing config-announce; adding it produced the grant.
- "master GRANT=0 for reac-pw" — an analysis script that labeled all `04 03` as
  cold-connect hid the master's grants. Always key `04 03` by L2 source (box JOIN
  vs master GRANT).
