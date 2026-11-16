#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
# THE omx-wire PROBE MATCHES A FRAME'S DEPARTURE WITH ITS ARRIVAL IN EITHER ORDER, AND CALLS ONLY A REAL LOSS A LOSS.
#
# tools/omx-wire-probe.py reads two sockets in turn, one at each end of a cable, so a frame's arrival is often read
# before its departure. Its first rehearsal on omx (veth, 2026-11-13) reported 14330 "dropped" frames on a wire whose
# receiving ends saw every counter step by one; then, one false loss per run at the window's start; then one per run on
# a reader that fell behind. Each was the probe, never the wire. This holds the matcher (class Direction) to the four
# orders and to the one loss that is real, with no socket, no root and no wire, so it NEVER SKIPS.
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("probe", os.path.join(HERE, "..", "tools", "omx-wire-probe.py"))
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
W = probe.MATCH_WINDOW_NS
failed = 0


def expect(cond: bool, what: str, d=None) -> None:
    global failed
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failed += 1
        if d is not None:
            print("     " + d.line())


# 1. departure read first, then its arrival: met, latency = rx - tx
d = probe.Direction("a->b")
for c in range(10):
    d.tx(c, 1000 + c * 125_000)
    d.rx(c, 1000 + c * 125_000 + 900)
d.expire(10_000_000_000)
expect(d.arrived == 10 and d.dropped == 0 and d.gaps == 0 and set(d.lat) == {900},
       "departure then arrival: every frame met, latency 900 ns", d)

# 2. arrival read first (the two sockets are read in turn): still met, same latency, no loss
d = probe.Direction("a->b")
for c in range(10):
    d.rx(c, 1000 + c * 125_000 + 900)
    d.tx(c, 1000 + c * 125_000)
d.expire(10_000_000_000)
expect(d.arrived == 10 and d.dropped == 0 and d.unmatched_rx == 0 and set(d.lat) == {900},
       "arrival read before departure: every frame met, latency 900 ns", d)

# 3. a frame that left and never arrived: one drop, one counter hole at the far end, the rest met
d = probe.Direction("a->b")
for c in range(10):
    d.tx(c, c * 125_000)
    if c != 4:
        d.rx(c, c * 125_000 + 900)
d.expire(9 * 125_000 + W + 1)
expect(d.dropped == 1 and d.gaps == 1 and d.missing == 1 and d.arrived == 9,
       "a frame lost on the wire: 1 dropped, 1 hole of 1 at the far end", d)

# 4. a reader that fell behind: the receiving end has not been READ past the window, so nothing is lost yet
d = probe.Direction("a->b")
d.tx(1, 0)
d.expire(W // 2)     # the newest stamp read at the far end is still inside the window
expect(d.dropped == 0 and 1 in d.pending, "an arrival not yet read is not a loss before the far end is read past it", d)
d.rx(1, 900)
expect(d.dropped == 0 and d.arrived == 1, "and it is met when it is read", d)

# 5. the 16-bit counter wraps: 65535 -> 0 is a step of one, not a hole
d = probe.Direction("a->b")
for i, c in enumerate([65533, 65534, 65535, 0, 1]):
    d.tx(c, i * 125_000)
    d.rx(c, i * 125_000 + 900)
expect(d.gaps == 0 and d.arrived == 5, "the counter wraps at 65535 without a hole", d)

# 6. sabotage: an arrival whose departure was never seen is counted apart, and is not a wire drop
d = probe.Direction("a->b")
d.rx(7, 900)
d.expire(900 + W + 1)
expect(d.unmatched_rx == 1 and d.dropped == 0, "an arrival with no departure: unmatched, never a drop", d)

print("omx-wire probe matcher %s (%d failed)" % ("FAILED" if failed else "ok", failed))
sys.exit(1 if failed else 0)
