<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# A dropped box wakes on a PHY edge — the wake ladder, and the one thing the master can still do

Status: RULED 2026-09-16 from the live failure in
`docs/design/notes/2026-09-16-a-box-that-never-came-back.md`; normative for `src/reac_wake.c`
and its actuation in `src/main.c`. AMENDS libreac's
`docs/design/specs/2026-09-14-capturing-a-linked-silent-box.md` — that spec's §4 rig run has now
been run and it FAILED for an S-1608. It does not overturn it: the completed push stays the
FIRST rung of the ladder, because the same spec measured it capturing an S-4000S.

## 1. What the log promised, and what the wire did

Since libreac 1.1.2 the PROBING watchdog tells the operator, every 10 s:

> A box that is LINKED AND SILENT looks like this — it goes quiet when its desk leaves and
> answers a COMPLETED scene push, **so do not bounce it yet.**

On 2026-09-16 the desk suspended for 77 minutes with an S-1608 enrolled. On resume the daemon
re-took the wire as MASTER and pushed correctly for **73 minutes across two processes — about
1620 completed scene transfers — for `rx_box_frames=0`**, with the NIC's own counters showing
8003 frames/s going out and **zero** coming back. The sentence above is what stopped anyone
acting for the first hour. It is right about the S-4000S it was written from and wrong as a
general law, and the daemon has been repeating it as one.

## 2. Why a completed push cannot be the whole answer

The box's own firmware says so, and it said so in this repo's tree the whole time.
`reac-firmware-re/REAC-PROTOCOL-FROM-SOURCE.md` §10.2, the decompiled S-1608 slave FSM:

    BOOT     --> ANNOUNCE: PHY LINK-UP (the only establish trigger; a data gap does NOT)
    LINKED   --> ANNOUNCE: PHY link-down/up -> re-cold-connect

and `reac-firmware-re/REAC-CONNECTION-FSM.md`: *"Box announces on PHY-up only, never on a data
gap — firmware-confirmed."* `DROP` leaves only through `PHY_UP`. A box that has torn its master
down has no edge back that a master can drive **with frames**, whatever those frames are — and
ours are byte-identical to a real desk's (libreac's own §2 table compares them field by field).

The live window carries its own positive control on the same wire: at 15:51:12, seventy-three
minutes into the silence, a freshly powered S-4000S answered our push and reached ESTABLISHED
inside seven seconds. The master was not broken. The difference between the two boxes at that
instant was a PHY edge.

## 3. The ruling — the wake ladder

The master owns the interface it drives (it already installs and removes that device's etf
qdisc over rtnetlink, and mints its VLAN children). So the PHY edge the box needs is an edge
the daemon can produce, on its OWN port, and it is the last rung of a ladder:

1. **PUSH.** Drive the wire and complete scene transfers. Cheap, silent, disturbs nothing, and
   it is measured to capture an S-4000S. Nothing else happens until it has been given a fair
   run — and "fair" is counted in COMPLETED PUSHES, not in seconds, because a clock says
   nothing about whether our own transfer ever finished.
2. **BOUNCE.** With carrier up, `rx_box_frames` still exactly 0 and our push proven complete
   `REAC_WAKE_MIN_PUSHES` times, take the master's own interface administratively down for
   `REAC_WAKE_DOWN_MS` and back up. That is the box's `PHY LINK-UP`, and it is the only event
   its firmware leaves `DROP` on.
3. **STOP, AND SAY SO.** After `REAC_WAKE_MAX_BOUNCES` attempts, never again on this segment:
   print what was tried and hand the operator the one remedy left (the cable, or the box's own
   power). A daemon that flaps a port forever is worse than one that waits.

## 4. The refusals — each is a fact, not a delay

`reac_wake` returns NONE with a named refusal, and the daemon says it once:

| refusal | why it is never a bounce |
|---|---|
| `NO_CARRIER` | there is no link to break; the cable is out and only the cable fixes it |
| `CARRIER_UNKNOWN` | an unreadable probe is not evidence; -1 is not 0 (libreac's own rule) |
| `BOX_IS_TALKING` | `rx_box_frames > 0`: the far end is alive and the fault is elsewhere |
| `NOTHING_SENT` | (2026-09-23) no frame of ours left the host since the last step: a box cannot have ignored a push it never received, and an edge would make it flood at a master that still cannot answer — the fault is on this side of the socket |
| `PUSH_NOT_PROVEN` | fewer than `REAC_WAKE_MIN_PUSHES` completed transfers: the cheap rung has not been played, and an incomplete push is measured to produce nothing (libreac §1's negative control) |
| `SETTLING` | inside `REAC_WAKE_SETTLE_NS` of a bounce: the box needs its flood (~1.36 s), its cold-connect and our grant dwell (~1.7 s) before it has failed to answer |
| `NOT_PROBING` | the master is granting or established; a bounce would tear down what works |
| `SIBLING_SERVED` | this device carries other served segments (its VLAN children): a bounce takes them down too, and we never break a working segment to wake a dead one |
| `EXHAUSTED` | the ladder is spent; only the operator can act now |

## 5. What the numbers are, and why

| constant | value | where it comes from |
|---|---|---|
| `REAC_WAKE_MIN_PUSHES` | 3 | the capture has the box answering **8.355 ms** after the last chunk (libreac §1), so three whole transfers is three times a proven-sufficient exposure, not a guess |
| `REAC_WAKE_GRACE_NS` | 12 s | ≥ 3 cycles at every rate this daemon serves (2.6945 s at 8000 fps, 2.9327 s at 3675 fps) — the clock is the FLOOR under the push count, never the trigger on its own |
| `REAC_WAKE_DOWN_MS` | 1200 | CHOSEN, and named as chosen: no capture measures how long a link must be down for an S-1608 PHY to register it. Longer than a 1 Gb autoneg cycle, and **under `REAC_IFSCAN_DOWN_HOLD_NS` (3 s)** — past that the daemon drops the segment out from under its own remedy and re-serves it from scratch. That ceiling is not a comment: `tests/test_reac_wake.c` asserts the two constants against each other, so a later edit to either is caught by the build |
| `REAC_WAKE_SETTLE_NS` | 20 s | the box's bounded flood (1.36 s) + cold-connect retries + our ~1.6 s ENROLL→grant dwell, with room for a slow autoneg, before a second attempt is honest |
| `REAC_WAKE_MAX_BOUNCES` | 2 | two edges are enough for a box that is there; a third is a port flapping at an empty socket |

## 6. What proves it

- `tests/test_reac_wake.c` — the ladder and every refusal, red first against today's behaviour
  (the daemon never bounces, and a box silent through four complete pushes is never woken).
- `tests/box-wakes-on-a-phy-edge.sh` — WHOLE-BINARY, in a private user+net+pid namespace on a
  veth pair with its own PipeWire: a fake box that models §2's firmware law (it answers NOTHING
  until it sees its own carrier drop and return, then floods and cold-connects) must be enrolled
  by the daemon with **nothing but the daemon acting**. Its positive control is the same box
  with the law relaxed, which must enrol without any bounce at all.
- What neither proves: that a REAL S-1608 answers the edge. That is one rig run — stop the
  daemon beside the dropped box, start it, and require `rx_box_frames > 0` and an enrolment with
  no cable touched — and it belongs to the main session.

## 7. Amendment 2026-09-23 — the boot that spent its edges on a wire carrying nothing

**The evidence** is `docs/design/evidence/reac-pw-boot-2026-09-23.log`, the user journal of the
13:43 boot, and it corrects the question this lane was opened on. The ladder DID fire: edge 1 of 2
on both `enp131s0` and `enp128s20f0u6` at 13:43:52 (lines 85, 87), edge 2 on `enp128s20f0u6` at
13:44:13 and "did not answer 2 PHY edges … nothing left to try" at 13:44:14 (lines 176-187). The
`reac-master:` lines carry no segment tag, and the `[N.442564]` watchdog that ran to "145
COMPLETED scene push(es)" belongs to the `enp128s20f0u6` master (its LINK-UP at `[82.442693]`
follows "'enp128s20f0u6' is back up", line 185-186), not to `enp131s0`. The S-1608
(`00:40:ab:c4:80:41`) is on `enp131s0` and joined at 13:44:01 (lines 148-169); the S-0808
(`00:40:ab:c4:dc:9c`) is on `enp128s20f0u6` and joined at 13:50:58 (lines 447-458) after the
operator's replug at 13:50:53.

**What the edges were made over.** The daemon started before chrony had set the kernel's TAI
offset; libreac refused the ETF backend ("the kernel's TAI offset is 0", lines 58, 69) and ran the
thread backend — under the etf root qdisc the daemon had ALREADY installed for the ETF default
(lines 54, 65; the arm precedes the open). With `skip_sock_check` that qdisc drops every unstamped
frame: `tx=0 tx_errors=81552` ten seconds in (lines 80, 83), 3 460 957 by the `enp128s20f0u6`
master's shutdown (line 425). The master's push counter counts chunks handed to the pacer, not
frames the kernel launched, so `scene_pushes` climbed over a wire carrying nothing of ours; the
ladder read the cheap rung as played, bounced twice, and gave up on a box that had never heard a
master. `enp131s0` recovered by ACCIDENT: its edge overshot the 3 s hold (a PCIe PHY renegotiates
slowly), the segment was dropped and re-served at 13:43:59 with a fresh pacer — by then TAI 37,
ETF ran, `tx_errors=0` (line 147) — and the S-1608 flooded and joined within two seconds. The
USB NIC's edge stayed inside the hold, so its dead pacer lived on until the replug re-served it
the same way and the S-0808 enrolled in one second.

**Three changes, one defect seen from three layers** (commit `wake: frames the kernel refuses
hold the ladder …`):

1. `reac_wake_obs.tx_frames` — frames that LEFT the host since the last step — joins the
   observation, and zero refuses the edge as `NOTHING_SENT` (§4 table) before any fact about the
   box is consulted. Proven pure in `tests/test_reac_wake.c` §I (an hour of pushes "completing"
   with `tx_frames` 0 makes no edge; the first frame leaving bounces).
2. **Refusals are now really said once.** §4 always read "the daemon says it once"; until today
   `main.c`'s `ACT_NONE` arm printed nothing, so a ladder holding for seven minutes was
   indistinguishable from one never asked. `reac_wake_refusal_to_say()` answers once per CHANGE
   of refusal (again after a reopen; never for OK), and the journal line is
   `the wake ladder on '<if>' holds: <refusal>`. Proven pure in §J.
3. **The qdisc follows the backend that actually opened.** If ETF was wanted and the pacer is
   not running it, the etf qdisc the daemon installed is removed again at once, with libreac's
   refusal quoted (`reac-qdisc: ETF was wanted on '<if>' and the pacer refused it (…) — the etf
   qdisc this daemon just installed is REMOVED again`). A thread-backend pacer then transmits with
   its looser cadence instead of transmitting nothing.

**Proven whole-binary** by `tests/wake-holds-when-nothing-is-sent.sh`: a thread-backend daemon on
an empty wire gets an etf root qdisc by hand three seconds in — `tx_errors` 300 890, no edge in
40 s, the refusal said exactly once; the control arm without the qdisc bounces the same wire
twice after "our own scene push has not completed yet" said once. Red on `1623184` (two edges,
the refusal said 0 times, 300 833 errors).

**Not proven here, for the desk.** (a) Whether an administrative down/up of the ax88179 USB NIC
(`enp128s20f0u6`) is a PHY edge the S-0808 sees at all: two edges were made and nothing was heard
back — but nothing of ours was on the wire either, so the box's flood after a real edge would
have been the only signal, and `rx_box_frames` stayed 0. One measurement settles it: with the
box enrolled, `ip link set enp128s20f0u6 down; sleep 1.2; ip link set enp128s20f0u6 up` and read
whether the box's REAC LED drops and the master logs `box presence GAINED (BCAST-FILLER …)`. (b)
The first-boot window itself: reac-pw starting before the TAI offset is set will still run the
thread backend for that process's life on those segments; the qdisc no longer eats the frames,
but the pacer does not re-try ETF later. A unit ordering (`After=chronyd.service` /
`time-sync.target`) or a periodic re-open is a separate ruling. (c) The S-1608's periodic
re-JOIN while established (`rx_joins=569` beside 30 M box frames in the 12:17-13:19 process, the
S-0808 at 2) is not this spec's subject and is not explained by anything here.
