# Establishment time: 1.7 s -> 0.2 s on both boxes, and why the knob shipped inert

Measured on the live rig 2026-08-30 02:15-03:10. Branch `test/grant-on-declare-on-rig-base`
= the branch the rig runs (`fix/the-binding-is-an-ifindex`) + a cherry-pick of `25d1853`
+ the fix in `8000142`, so the only difference from the rig's own daemon is this work.

## Result

| box | code path | control | `REACPW_GRANT_ON_DECLARE=1` | saved |
|---|---|---|---|---|
| S-1608 16/8 `…80:3b` | no window restart | 1.684 s | **0.134 s** | 1.55 s |
| S-4000S 32/8 `…08:bc` | window restart | 1.756 s | **0.206 s** | 1.55 s |

Three trials per arm, identical to the millisecond. Full port count every time (16 and 32
capture ports), wire at 8007-8010 pkt/s, `/stagebox` `ready` for both. **Control reproduced
each box's live baseline exactly**, so the fix does not move the default.

## Why the knob did nothing before

`25d1853` ends the dwell on the box's declaration. It worked — instrumenting the predicate
showed it TRUE from slot 401 with a 5 s cap. But the master still reached ESTABLISHED at the
full 5.16 s, because the grant burst was anchored to the dwell CONSTANT:

```c
gt = grant_ticks - 1 - grant_dwell;                                  /* negative when skipped */
grant_delivered = grant_ticks >= grant_dwell + burst_len*stride + 1;  /* waits out the cap */
```

Skipping the dwell drops into the burst branch with a negative cursor, so no grant slot is
ever taken and the FSM waits out a window nobody is using. **Emitting a grant early and
ARRIVING early are different things, and only the second one is audio.**

`dwell_ended_tick` latches the slot the dwell really ended at; `grant_dwell_anchor()` feeds
both call sites and falls back to `grant_dwell` when the dwell ran its full length. It resets
with the grant window (`enter_granting`, and `set_box`'s restart) so a stale anchor cannot
leak into a later session.

## What the test had to get right

`tests/test_reac_grant_dwell.c` drives the rig's sequence as a pure FSM and runs twice under
different env (the knob getters cache in statics, so one process = one configuration).

**Two earlier versions of this test passed before the fix**, which is the part worth keeping:

1. asserting on the first GRANT emission rather than on reaching ESTABLISHED — the grant DOES
   go out early, so the test measured something adjacent to the claim;
2. omitting the box's REPEATED announce. The live log carries two `box JOIN seen` lines 250 us
   apart, and `reac_pacer` calls `set_box` on each. The FIRST call restarts the grant window
   and clears `enroll_pending`; LATER calls re-arm it WITHOUT restarting. Without the second
   call the early-exit predicate can never arm and the harness exercises a path the rig never
   takes.

Sabotage-verified: reverting `grant_dwell_anchor()` to `grant_dwell` puts the fast arm red.
Suite 46 Ok / 0 Fail / 1 pre-existing skip.

## SUPERSEDED — the earlier conclusion in this file was wrong

An earlier revision claimed `enroll_sent_tick` was stuck at 0 because 0 is both a valid tick
and the "never sent" sentinel. **That was a code reading, and the rig refuted it**: the probe
printed `enroll_sent_tick=1`, `would_exit=1`. The sentinel collision is real in the source but
is not what defeated the knob. The burst anchor was.

`REACPW_GRANT_DWELL_MS=300` (measured earlier the same night, 0.456 s on the S-4000S) is
superseded as a route to the same saving: it is a blunter constant and breaks the box that
needs a long hold, which is exactly why `25d1853` chose an event. Keep the knob as the CAP.

## NOT PROVEN — what a default change still needs

- **No audio was measured.** Ports and packet rate are not audio through the box. Run the
  oracle before promoting this to the default.
- Two boxes and two firmwares, three trials each. The file's own warning stands: a box
  elsewhere needed ~27 s, and 1.6 s already grants too fast for it. Grant-on-declare is
  event-driven and so should be correct there — `grant_dwell` remains the cap for a box that
  has not declared — but that box has not been tested.
- The knob is still default-off. Promoting it is a separate decision with its own rig run.
