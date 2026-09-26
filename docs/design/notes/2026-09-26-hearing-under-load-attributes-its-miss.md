<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# The hearing test's trunk arm attributes its miss before it fails (#99)

2026-09-26. Status: **the test change is made; the run under desk load is NEEDS-DESK.**

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
