#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Trace the TX frame ring's depth from OUTSIDE the daemon, with no rebuild and no
# restart.
#
# reac-pw already publishes the drain-observed ring depth as SPA_PARAM_ProcessLatency
# on its playback node (reac_lat.c), so the sawtooth that ends in a 64 ms discard is
# visible to any PipeWire client. This just reads it on a timer. Passive: pw-dump
# takes no locks the daemon cares about and writes nothing.
#
#   ./tools/ring-depth.sh <node-id> [seconds] [interval]
#
# Find the node id with:  pw-dump | grep -n reac-playback
# Output: "<monotonic-ish epoch> <depth samples> <depth ns>" per poll, one line each.
# Depth in FRAMES is samples/12; the guard trims at 512 frames down to 256, so a
# fall of ~3000 samples is one discard of 64 ms of audio.
set -o pipefail
NODE=${1:?usage: ring-depth.sh <node-id> [seconds] [interval]}
DUR=${2:-300}
IVL=${3:-2}
END=$(( $(date +%s) + DUR ))
seen=0
while [ "$(date +%s)" -lt "$END" ]; do
  line=$(pw-dump "$NODE" 2>/dev/null | python3 -c "
import json,sys,time
d=json.load(sys.stdin)
for o in d:
    pl=o.get('info',{}).get('params',{}).get('ProcessLatency')
    if pl: print(f'{time.time():.3f} {pl[0][\"rate\"]} {pl[0][\"ns\"]}')
")
  if [ -n "$line" ]; then echo "$line"; seen=1; fi
  sleep "$IVL"
done
# A scan that found nothing and a scan that could not see are the same silence.
if [ "$seen" = 0 ]; then
  echo "ring-depth.sh: node $NODE published no ProcessLatency in ${DUR}s." >&2
  echo "  That is NOT evidence the ring is empty — check the node id first:" >&2
  echo "    pw-dump | python3 -c \"import json,sys; [print(o['id'], o.get('info',{}).get('props',{}).get('node.name')) for o in json.load(sys.stdin) if o.get('type','').endswith('Node')]\" | grep reac" >&2
  exit 1
fi
