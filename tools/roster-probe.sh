#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# READ the segment roster off whatever PipeWire graph this shell can see, the way an
# operator does. Nothing here writes: one pw-dump, one filter.
#
#   tools/roster-probe.sh            # the roster node of every reac-pw on this graph
#   tools/roster-probe.sh <pid>      # only the one belonging to that process
#
# Exit 0 = a roster node carries reac.roster.n and that many complete groups.
# Exit 1 = a roster node exists and is EMPTY, or a group is short a key -- the live defect
#          of 2026-09-16 (node 188 on the rig: node.name, media.class, reac.roster, and
#          nothing else, three established segments and three minutes in).
# Exit 2 = no roster node at all, or no graph to ask.
#
# WHY A SCRIPT. The defect above was invisible to a test that reads its own private graph
# and visible in one pw-cli info; the difference between the two must be one command, not a
# remembered incantation. Read `reac.roster` with str(): pw-dump renders a property whose
# value looks numeric as a JSON NUMBER, so `== "1"` on a string property is false and the
# node reads as absent -- measured while writing this.
set -u
PID="${1:-}"
command -v pw-dump >/dev/null 2>&1 || { echo "roster-probe: no pw-dump"; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "roster-probe: no python3"; exit 2; }
pw-dump 2>/dev/null | python3 -c '
import json,sys
want = sys.argv[1] if len(sys.argv)>1 and sys.argv[1] else None
try:
    d=json.load(sys.stdin)
except Exception as e:
    print("roster-probe: no graph to read (%s)" % e); sys.exit(2)
mine=None
if want:
    mine={o["id"] for o in d if o.get("type")=="PipeWire:Interface:Client"
          and str(o["info"]["props"].get("application.process.id")) == want}
found=bad=0
for o in d:
    if o.get("type")!="PipeWire:Interface:Node": continue
    i=o["info"]; p=i["props"]
    if str(p.get("reac.roster")) != "1": continue
    if mine is not None and int(p.get("client.id",-1)) not in mine: continue
    found+=1
    ports=int(i.get("n_input_ports",0))+int(i.get("n_output_ports",0))
    n=p.get("reac.roster.n")
    print("node %s %s ports=%d reac.roster.n=%s" % (o["id"], p.get("node.name","?"), ports,
          n if n is not None else "(ABSENT)"))
    if n is None:
        print("  EMPTY: this node carries no roster at all — the deriver never published")
        bad+=1
        continue
    for k in range(int(n)):
        row=["%s=%s" % (f, p.get("reac.roster.%d.%s" % (k,f), "(ABSENT)"))
             for f in ("name","state","model","role","source","width")]
        print("  [%d] %s" % (k, "  ".join(row)))
        if any("(ABSENT)" in r for r in row): bad+=1
    stray=[k for k in p if k.startswith("reac.roster.") and k != "reac.roster.n"
           and k.split(".")[2].isdigit() and int(k.split(".")[2]) >= int(n)]
    if stray:
        print("  STRAY: %d key(s) past reac.roster.n=%s: %s" % (len(stray), n, sorted(stray)[:6]))
        bad+=1
if not found:
    print("roster-probe: NO node carrying reac.roster=1 on this graph")
    sys.exit(2)
sys.exit(1 if bad else 0)
' "$PID"
