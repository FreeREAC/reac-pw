# grant-on-declare is INERT on the S-4000S, and the dwell it targets is safe to shorten

Measured 2026-08-30 02:15-02:30 on the live rig, S-4000S `00:40:ab:c4:08:bc` on `enp131s0`,
96 kHz. Branch `test/grant-on-declare-on-rig-base` = the rig's own
`fix/the-binding-is-an-ifindex` plus a cherry-pick of `25d1853`, so the only difference from
what the rig runs is grant-on-declare itself. Suite on this build: 44 Ok / 0 Fail / 1 skip.

## Result

| run | knob | grant dwell | ended by |
|---|---|---|---|
| live binary (tonight's cold-connect) | — | **1.756 s** | timer |
| test binary | unset | **1.756 s** | timer |
| test binary | `REACPW_GRANT_ON_DECLARE=1` | **1.756 s** | timer |
| test binary | `REACPW_GRANT_DWELL_MS=300` | **0.456 s** x4 trials | timer |

Knob-off reproduces the live binary to the millisecond, so "default off and byte-identical
unset" holds. **`REACPW_GRANT_ON_DECLARE=1` changes nothing on this box.** The box declares at
+0.852 s and we still grant at +1.756 s — 0.904 s left on the table, the exact gap the knob
exists to close.

## Why — `0` is both a valid tick and the "never sent" sentinel

`reac_master.c`, the block that runs when a box declares while we are holding an ungranted
window (`!had_box && state == REAC_M_GRANTING`):

```c
m->grant_ticks      = 0;
m->enroll_sent_tick = 0;
m->enroll_pending   = 0;   /* the restarted window's tick-0 ENROLL carries the width */
```

The restarted window then emits its ENROLL through the `grant_ticks == 0` branch, and **that
branch never sets `enroll_sent_tick`**. So it stays `0` for the rest of the session, and the
early-exit test

```c
grant_on_declare() && !m->enroll_pending && m->enroll_sent_tick > 0 && ...
```

can never be true. The ENROLL really is sent at tick 0, so the honest value and the absence
marker collide.

**The S-1608 does not take the window-restart path and the S-4000S does**, which is why the
branch measured green on the S-1608 and is dead here — on the box class that actually REQUIRES
ENROLL (its FSM needs subtype 0x10 to leave state 3; the S-1608 self-places).

The fix is to record "an enrol was sent" in something that can express absence — a flag, or a
sentinel that is not a reachable tick. Do not simply set `enroll_sent_tick = m->grant_ticks` in
the tick-0 branch: that writes `0` and changes nothing.

**The knob logs nothing when it fires or fails to.** An experiment knob that can silently do
nothing is unfalsifiable — an hour of it being inert looked exactly like success. Whatever the
fix, it should say which path ended the dwell; `GRANTING -> ESTABLISHED (timer)` vs `(declared)`
would have made this measurable in one run instead of a code read.

## The dwell itself is safe to shorten on this box — 1.300 s, available without the fix

`REACPW_GRANT_DWELL_MS=300` gave **0.456 s, four trials, identical to the millisecond**, and the
box enrolled at FULL WIDTH every time: 32 capture + 8 playback ports, `/stagebox` `S-4000S
in=32 out=8 ready`, `/reac/segment` `seg2: established 96000Hz model=s4000s`, wire 8005 pkt/s.
That answers this file's own open question for this firmware — the box TOLERATES the short
dwell, it does not require the 1.6 s hold.

**Not yet proven, and needed before this becomes a default:**

- **No audio was measured.** Ports existing is not audio flowing, and this file records the
  S-4000 once being stuck at 8ch. Run the oracle before believing the width.
- Four trials is a reliability signal, not a proof, and only for THIS firmware. The comment's
  warning stands: a box elsewhere needed ~27 s and reac-pw's 1.6 s already "grants too fast" for
  it. Per-firmware proof, as the file says.
- The S-1608 was not tested with a short dwell; it ran throughout on the rig's own daemon.
