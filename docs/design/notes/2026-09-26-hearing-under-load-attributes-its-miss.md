<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# The hearing test's trunk arm attributes its miss before it fails (#99)

2026-09-26. Status: **the trunk arm waits on its conditions under one hang guard (§4); the
2026-09-14 `ok=1` symptom itself is not reproduced off the desk, and the desk run is NEEDS-DESK.**

## 1. What was left

`reac_hearing_finds_a_segment` went red on 2026-09-14 while a full openmixer gate loaded the
desk (load ~2.8): `FAIL: trunk0.12 is up and decoded no audio (ok='1')`. Rerun alone minutes
later, it passed. 1.0.3 added a settle (30 × 0.4 s, `ok > 100`) and a telemetry dump. The
2026-09-17 recheck on the issue named what the settle did not do: nothing in the failing arm
asked whether the **fake** was still sending, so a fake that stalled and a slave that went
deaf still read the same, and both were blamed on the daemon.

## 2. What changed

- The two VLAN fakes (`tbox0.11`, `tbox0.12`) now get a report file. Its `tx` line is the
  fake's own frame counter, written every ~300 ms (`tests/fake_box_master.c`, `ear_report`).
  The ear socket is opened either way, so the report changes only whether a snapshot is
  written, not how the fake behaves on the wire.
- The audio check reads `tx` at the start and the end of the **same** 12 s window it waits
  on the daemon's `ok=` counter:
  - fake sent **more than** the 100 frames the check asks for, daemon decoded ≤ 100 →
    **FAIL**, as before, now also printing `tx a -> b` so the reader knows the frames were
    there;
  - fake sent ≤ 100 in the window → **SKIP (77)**, naming the counts and whether the fake
    process is gone;
  - the fake's report cannot be read → **FAIL**. A harness that cannot see its fake does not
    get to call it stopped.
- The daemon's assertion is unchanged: same threshold, same window, same segments.
- The fake counts a send the kernel refused with `ENOBUFS` as sent. That can only turn a
  SKIP into a FAIL, never the reverse.

The decision logic was exercised with a stubbed log and report (fake live → FAIL, fake
stalled → SKIP, no report → FAIL, daemon decoding → pass).

## 3. Why it is NEEDS-DESK

The arm needs a kernel that can create 802.1Q links in a namespace. The container this was
written in has no `8021q`, and the whole test SKIPs there at its own probe. So the changed
arm has **not been run** at all yet, loaded or idle. Two things are still open, and neither
can be settled without a desk:

1. **A plain run on a desk with 8021q**, idle, to show that the reports appear and the arm
   stays green (`tx` readable, `ok > 100`).
2. **The 2026-09-14 condition**: the full suite under an openmixer gate at load ~2.8. If it
   goes red again, the new output says which case it is. A **SKIP** means the fake was
   starved. A **FAIL** with `the fake WAS sending` means a real daemon defect: the slave on a
   minted VLAN decoded one frame out of a live stream. That is its own issue, and
   [`2026-09-22-a-starved-poll-called-a-busy-wire-silent.md`](2026-09-22-a-starved-poll-called-a-busy-wire-silent.md)
   is the nearest known mechanism to check first.

#99 stays open until (1) is green and (2) has survived three gated builds, the bar its own
2026-09-14 comment set.

## 4. The arm waits on what it asserts, never on a clock

Ruling: a test fails on time only when time is what it tests. Nothing in the trunk arm
measures time, and three of its waits were clocks:

- eleven `wait_for … 20`/`25` deadlines on journal lines;
- `sleep 1.5`, then ONE read of the three VLAN nodes;
- the audio check's 12 s receive window (30 × 0.4 s) for `ok > 100`.

Each now ends on its own condition: the line appears, the nodes carry the segment, box
address and width the assertions ask for, the feeder has decoded more than 100 frames.
`TRUNK_HANG` (120 s) is the one wall-clock number left in the arm, and it is a hang guard.
The assertions after the node wait run once on the last read, so a node that never got
there still fails by its own name. The tx attribution of §2 is read across the same wait.

### Measured (an omx-worker pod: 8-CPU quota on 16 cores, user+net namespaces without the
pid namespace, whose `--mount-proc` the pod's masked `/proc` refuses)

| load on the base tree | runs | result |
|---|---|---|
| a busy loop per core (8, then 16) at nice 0, whole run | 8 | 8 green |
| test pinned to 2 cores, 2 loops on each, test at nice 0 / nice 19 | 3 / 3 | 3 green / 3 red in the cold0/cold1 arms (frame-rate checks, where time is what is tested) |
| the same, trunk arm only, 2 and 6 loops per pinned core, test at nice 19 | 5 | 5 green, trunk feeders decoding 40-100 frames/s |
| the vid-12 fake SIGSTOPped 1 s as trunk0.12's slave starts | 1 | green, gate locked on the fake, `other=0` |
| test's process group run 4 ms in every 604 ms (SIGSTOP duty cycle) in the trunk arm, a loop per core | 2 | **2 red**: `trunk0.11 was created and never served as a segment` at the 25 s wait, trunk0.11's feeder at `ok=1023` |
| that same load on this tree | P20 | P20R |

Sabotage, each once, with `TRUNK_HANG=15`: the expected trunk1.13 box address made wrong →
the node wait runs to the guard and the arm fails `the adopted VLAN's node carries box.mac`;
the daemon's `ok=` line made unreadable → the audio wait runs to the guard and the arm fails
`trunk0.11 is up and decoded no audio (ok='') … the fake WAS sending: tx 5375 -> 31084`.

Two mechanisms for "one frame, then nothing" are ruled out by measurement: libreac's
capture socket does not see the slave's own flood (`other=0` throughout, so the
upstream gate cannot lock onto our own address), and the telemetry's 2 s clock is per
feeder thread, not shared. No CPU load here reproduced `ok=1` for a whole window; the
nearest known mechanism, a starved poll calling a box-mastered wire silent and re-serving
it, was fixed after 2026-09-14 (`e32d225`). The desk runs of §3 stay open.
