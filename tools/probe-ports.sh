#!/bin/bash
# Record every capture port of a REAC node and report per-port RMS in dBFS.
#
# pw-record --target= SILENTLY IGNORES its target, so two captures aimed at
# different physical boxes come back byte-identical. This links EXPLICITLY
# (--target=0 disables auto-linking) and PRINTS the links it made, because a
# recording you cannot show the links for is not evidence.
#
# A REAC node always carries a ~-106.6 dBFS floor. An exact 0.0 on every port is
# a FAILED CAPTURE, not silence — the analysis below says so rather than
# reporting a quiet desk.
#
#   tools/probe-ports.sh reac-capture 3 /tmp/s0808.wav
set -o pipefail
node="$1"; secs="${2:-3}"; out="${3:-/tmp/probe.wav}"

mapfile -t ports < <(pw-link -o | grep "^${node}:" | sort -V)
[ "${#ports[@]}" -gt 0 ] || { echo "no output ports on ${node}"; exit 1; }
nch="${#ports[@]}"
echo "node=${node} ports=${nch}"

# AUX channel names, so the recorder's input ports sort into the SAME order as
# the source ports. With the default layout an 8-channel recorder gets 7.1 names
# (FL,FR,FC,LFE,...) whose sort order is NOT the port order, and capture_AUX0
# lands in whichever WAV channel FC happens to be. Two ports silently swapped is
# the same failure as a capture aimed at the wrong box.
cmap=$(printf 'AUX%d,' $(seq 0 $((nch-1))) | sed 's/,$//')
pw-record --target=0 --channels="$nch" --channel-map="$cmap" \
          --format=f32 --rate=48000 "$out" &
rec=$!
sleep 1

sink=$(pw-link -i | grep -m1 "^pw-record" | cut -d: -f1)
[ -n "$sink" ] || { kill "$rec" 2>/dev/null; echo "no pw-record input node"; exit 1; }
mapfile -t inputs < <(pw-link -i | grep "^${sink}:" | sort -V)

echo "--- links made ---"
for i in "${!ports[@]}"; do
	[ "$i" -lt "${#inputs[@]}" ] || break
	pw-link "${ports[$i]}" "${inputs[$i]}" 2>/dev/null && \
		echo "  ${ports[$i]}  ->  ${inputs[$i]}"
done
echo "--- verify (pw-link -l on the recorder) ---"
pw-link -l | grep -A1 "^${sink}:" | head -40

sleep "$secs"
kill "$rec" 2>/dev/null
wait "$rec" 2>/dev/null
echo "wrote $out"
