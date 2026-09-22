<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# A starved poll called a busy wire silent

2026-09-22. `box-is-not-re-decided` was red at `b53edd6` and its CONTROL arm was what failed:
"the control wire's box master was never joined". The property the test is named for was never
in question, and nothing here changes a design — the daemon was taking a wire that a box master
fills at 2000 fps, against
[`2026-09-16-auto-role-per-segment.md`](../specs/2026-09-16-auto-role-per-segment.md) §1, where
case (a) is *"no frame for the listening window"* and case (c) is *slave-join the box*.

## 1. What the run did

Two segments, one daemon: `nbx0` pinned `role = box` on a silent wire, `ngn0` in `auto` with
`fake-box-master` flooding it from before carrier. `ngn0` heard the box, and then took the wire:

```
S_SEGMENT_HEARD [ngn0] REAC heard — unknown 00:40:ab:c4:dc:a2 (8 ch): this interface is a segment
[nbx0] role = box — presenting S-1608 …                       (nbx0 is served here)
[ngn0] no REAC heard in 500 ms — … taking it as MASTER and probing until a box cold-connects
[ngn0] a box masters this segment — … yielding the master role and joining as SLAVE
```

The yield is the daemon catching its own mistake, and it costs the segment a full master
establishment — an `etf` qdisc installed and removed, a pacer started and shut down, the pair
rebuilt — before it joins the box it had already heard. The join line the test waits for
(`box masters this wire`) never comes, because by then the verdict is a yield and not a join.

## 2. The measurement

A temporary print of the knock's own fields at the decision point, and the sniffer's frame
counter beside them:

```
DBG [ngn0] knock st=2 … heard=95043018441642 now=95043018574764 frames=349
DBG [ngn0] knock st=2 … heard=95043018441642 now=95043548253397 frames=349
[ngn0] no REAC heard in 500 ms …
```

529.7 ms between two polls of a loop that polls every 200 ms, and the frame counter did not
move across the gap on a wire carrying 2000 fps. The only work in the gap is `nbx0` being
served. `last_heard_ns` advances in the sniffer's socket callback, and serving a segment runs
to completion inside the same loop, so the licence's window closed over frames that were
sitting unread in the socket queue. The observation was of the daemon's attention, not of the
wire.

## 3. Why it appeared on 2026-09-21 and not before

`1060d83` — *a cancelled masterless observation re-opens when the wire goes quiet again* — is
right, and it is what libreac 1.4.0 carries (`reac_knock.h`, and `test_reac_enrolment_binding.c`
requires it of the linked library). Before it, `REAC_KNOCK_CANCELLED` was terminal: the first of
those 349 frames ended the observation for the life of the process, so a starved poll could not
resurrect it. The latch was hiding this defect, not preventing it — a poll gap before the FIRST
frame would have driven the wire just the same.

Measured: green at `4e94d8f`, red at `1060d83`, red at `b53edd6`, in a throwaway worktree with
the same libreac 1.4.0 installed.

## 4. The fix

`sniffer_drain()` is called at the point of decision in `hearing_hunt`, before the tap-authority
wait and the licence are read. Every frame that arrived is then either already stamped by the
callback or is still queued and is stamped now, so silence at that point is the wire's silence.
It also feeds `reac_hunt_observe`, so the sighting a starved poll delayed lands in the table in
the same step.

Nothing moves in libreac: the licence is pure and answered exactly what it was given. What was
wrong is what the daemon gave it.

## 5. Owed

The starvation itself is untouched. Serving one pinned box segment blocks the hearing loop for
about half a second, and every other deadline in that loop is exposed to it in the same way.
