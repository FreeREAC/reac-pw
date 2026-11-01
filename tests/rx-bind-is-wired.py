#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""The slave side's receive gate is locked where main.c opens and ticks a listener.

tests/test_reac_rx_bind.c proves the lock does what it should; it cannot see whether
main.c makes the calls. Three facts, each read off src/main.c:

  1. listener_open locks the gate (reac_rx_bind_hint) after reac_rx_open. The feeder thread
     is started by listener_open's callers, never inside it, so the lock is in place before
     the first frame is read; this asserts that stays true.
  2. listener_publish_segment ticks the learned MAC (reac_rx_bind_tick), and both calls are
     guarded to the DOWNSTREAM gate (a box-on-M join receives upstream and is not bound).
  3. the hunt's MAC for a desk we slave to is carried into listener_cfg
     (bind_mac_set), and only for a SLAVE verdict.
"""
import re
import sys

src = open(sys.argv[1] + "/src/main.c").read()


def body(name):
    m = re.search(r"^static [^\n]*\b" + name + r"\(struct listener \*L[^\n]*\)\n\{", src, re.M)
    if not m:
        sys.exit("FAIL: no function " + name)
    i, depth = m.end(), 1
    while depth:
        c = src[i]
        depth += (c == "{") - (c == "}")
        i += 1
    return src[m.end():i]


bad = []
lo = body("listener_open")
o, h = lo.find("reac_rx_open("), lo.find("reac_rx_bind_hint(")
s = lo.find("reac_rx_start(")
if h < 0:
    bad.append("listener_open never calls reac_rx_bind_hint")
elif not (0 <= o < h):
    bad.append("reac_rx_bind_hint must sit after reac_rx_open")
if s >= 0:
    bad.append("listener_open starts the feeder itself: the lock would have to precede it")
pub = body("listener_publish_segment")
if "reac_rx_bind_tick(" not in pub:
    bad.append("listener_publish_segment never calls reac_rx_bind_tick")
elif not re.search(r"REAC_RX_ACCEPT_DOWNSTREAM\)\s*\n\s*reac_rx_bind_tick\(", pub):
    bad.append("the tick must be guarded to the DOWNSTREAM gate: a box-on-M join is not bound")
if not re.search(r"bind_mac_set &&[^\n]*\n[^\n]*REAC_RX_ACCEPT_DOWNSTREAM\)\s*\n\s*reac_rx_bind_hint\(", lo):
    bad.append("the hint must be guarded to the DOWNSTREAM gate: a box-on-M join is not bound")
if not re.search(r"REAC_HUNT_SLAVE[^{]*\n?[^{]*bind_mac_set\s*=\s*1|"
                 r"hunt->verdict == REAC_HUNT_SLAVE &&\s*\n\s*hunt->arb\.state == REAC_SEGMENT_FOREIGN"
                 r" && hunt->arb\.have_mac", src):
    bad.append("the hunt's rival MAC is not carried into the listener cfg for a SLAVE verdict")
if "bind_mac_set = 1" not in src:
    bad.append("bind_mac_set is never set")
for b in bad:
    print("FAIL:", b)
sys.exit(1 if bad else 0)
