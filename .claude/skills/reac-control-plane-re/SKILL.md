---
name: reac-control-plane-re
description: Discipline and workflow for reverse-engineering and rig-testing the REAC 0x8819 control plane against real Roland hardware (stageboxes, M-200/M-300/M-5000). Use for any reac-pw/libreac control-plane, head-amp, establishment or capture work.
---

# REAC control-plane RE — discipline and workflow

This skill is **method, not facts**. Every protocol fact lives in a committed,
evidence-cited document; this file tells you how to work and where to look. If
you find a fact here, it is a bug — move it to the right document.

## Where the facts live (read these, do not re-derive)

| question | authority |
| --- | --- |
| Frame layout, control multiplex, DT1 records | `reac-protocol` → `spec/reac.ksy` (formal, CI-validated against libreac) + `wire-format.md` |
| Byte layout in code | **libreac** — the executable oracle (`reac_braid.h`, `reac_upstream.h`, `reac_frame_clean_len()`). Never fork a second decoder. |
| Master establishment states | `docs/MASTER-FSM.md` (diagram + transition table) |
| Slave/box establishment | `docs/REAC-BOX-STATE-DIAGRAM.md`, `docs/REAC-CONNECTION-FSM.md` |
| Slot placement (per-model bases) | `docs/PLACEMENT-EVIDENCE.md` — 82 captures; the base is **negotiated**, the carrier is not yet isolated |
| The two address spaces | `src/reac_slots.h` — audio fabric = 40 slots; head-amp/chanmap = 48 (`0x00..0x2f`). **Never conflate them.** |
| Environment knobs | `docs/ENV-KNOBS.md` (and `--help`). Main carries exactly two. |
| Duplicate-frame guard, OHRCA +2 | `docs/OHRCA-UPSTREAM-DUPLICATE-FRAMES.md` |
| Captures + analysis tooling | `reac-captures/` (corpus) and its `analysis/` (`reac_pcap.py` streaming parser — reuse it, do not write another) |

Private reverse-engineering material (firmware decompiles) stays in
`reac-firmware-re` and **never** leaks into a public repo.

## Non-negotiable discipline

1. **Measure, never theorize.** Every claim about hardware behaviour comes from a
   capture, a running process, or a file — not from reasoning. Plausible-and-wrong
   has cost this project weeks; a wire capture costs minutes.
2. **No hardware claim from a soft meter.** "48 V is on" means a condenser mic
   comes alive or a meter reads at the XLR pins. An LED, a register readback, or a
   level bar in the UI is not proof. A dynamic mic is the positive control.
3. **Never mask a real desync.** If the box is cycling, do not extend timeouts or
   fake presence so the log *says* established — that makes the software lie. Fix
   the cause or report the failure honestly.
4. **Follow the real desk.** A captured M-200/M-300/M-5000 is the reference for
   ORDER and BYTES. Do not invent frames a real console never sends.
5. **One change per rig cycle.** Stack variables only to isolate, and write down
   what each one did.
6. **The goldens are the oracle.** If a refactor changes a golden, the refactor is
   wrong — never regenerate a golden to make a diff go away.

## Two checksums, in the right order

An op-`0403` record carries a **Roland DT1 (inner)** checksum *and* the REAC
**block (outer)** checksum. Stamp the inner one first, then the outer
(`reac_ctrl_record_cksum_stamp()` then `reac_ctrl_block_cksum_stamp()`). A record
finished with only the block helper looks perfect on the wire and is rejected by
the box.

## Rig topology

- The master must run on a **dedicated, non-mirrored** switch port. A mirrored
  port delivers every frame twice; reac-pw drops byte-identical duplicates
  (`REAC_DEBUG=1` → `dup=`), but a non-zero `dup` count means the topology is
  wrong, not that the guard is working.
- **Single master per segment.** Before launching, confirm with a short capture
  that no other master's MAC is on the wire.
- Confirm the interface, MACs and box model **every session** — the rig drifts,
  and a stale note in someone's head is how the duplicate-frame bug survived for
  days.

## The loop

```bash
cd ~/Devel/audio/reac-pw
ninja -C build && sudo -n setcap cap_net_raw,cap_sys_nice+ep build/reac-pw
pkill -x reac-pw                 # -x ALWAYS: pkill -f '<pattern>' matches the
                                 # killing shell's own cmdline and kills it (exit 144)
REAC_DEBUG=1 build/reac-pw --live <iface> --role master ... 2>&1 | tee /tmp/run.log
```

Success is measured on the FSM transcript, not on vibes:

```bash
grep -c 'GRANTING -> ESTABLISHED' /tmp/run.log   # re-establishments
grep -c 'ESTABLISHED -> PROBING'  /tmp/run.log   # drops — want 0 over a long run
grep 'reac_rx: ok=' /tmp/run.log | tail -1       # ok / dup / other / bad / gaps
```

Offline first: `meson test -C build` is deterministic and needs no hardware. A
change that cannot be pinned by a golden or a fixture usually is not understood
yet.

## Gotchas that have actually bitten

- `pkill -f '<pattern>'` self-kills the shell — always `pkill -x <exename>`
  (same for `tcpdump`).
- The box streams **audio** even when its control heartbeat stops: "I still hear
  it" does not mean the link is established.
- Multi-GB captures fill tmpfs fast — slice with `tcpdump -c`, delete as you go.
- reac-pw `main` **auto-deploys to the live rig on merge**. Merge only what is
  rig-verified or provably byte-identical (tests, docs, fixtures).
