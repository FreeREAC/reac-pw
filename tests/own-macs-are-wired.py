#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""The host's own addresses are screened out where main.c reads the wire (#8).

tests/test_reac_own_macs.c proves the screen; it cannot see whether main.c makes the calls.
Two facts, each read off src/main.c:

  1. sniffer_drain screens every frame (reac_own_echo_screen) before it reaches the hunt
     (reac_hunt_observe).
  2. the address set is read when a sniffer opens (sniffer_open_ex) and when the interface table changes
     (hearing_apply), so a NIC that appears later is ours from then on.
"""
import re
import sys

src = open(sys.argv[1] + "/src/main.c").read()


def body(name):
    m = re.search(r"^static [^\n]*\b" + name + r"\([^\n]*\)\n\{", src, re.M)
    if not m:
        sys.exit("FAIL: no function " + name)
    i, depth = m.end(), 1
    while depth:
        c = src[i]
        depth += (c == "{") - (c == "}")
        i += 1
    return src[m.end():i]


bad = []
drain = body("sniffer_drain")
s, o = drain.find("reac_own_echo_screen("), drain.find("reac_hunt_observe(")
if s < 0:
    bad.append("sniffer_drain never calls reac_own_echo_screen")
elif not 0 <= s < o:
    bad.append("reac_own_echo_screen must come before reac_hunt_observe")
for fn in ("sniffer_open_ex", "hearing_apply"):
    m = re.search(r"^static [^\n]*\b" + fn + r"\(", src, re.M)
    if not m:
        bad.append("no function " + fn)
    elif "reac_own_macs_read(" not in body(fn):
        bad.append(fn + " never re-reads the host's addresses")
for b in bad:
    print("FAIL:", b)
sys.exit(1 if bad else 0)
