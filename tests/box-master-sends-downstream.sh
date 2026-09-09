#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY, END TO END: a stagebox on M masters a wire, the daemon joins it, and the
# BOX'S OUTPUTS ARE PLAYABLE FROM THE GRAPH. The operator's job, measured at the far end of
# the cable rather than argued from the journal — a tone is played into
# `reac-playback.<segment>` through a real session manager, and the emulator decodes it back
# out of the frames the daemon sent, through libreac's own decoder.
#
# THE RULING THIS PROVES (2026-09-09, DESIGN.md "## 0.5.5"): "Sending is always the same,
# being clock slave is only part of the enrollment." Until 0.5.4 this wire was joined
# RECEIVE-ONLY and tests/hearing-finds-a-segment.sh asserted that the daemon put NOTHING on
# it. That assertion is now the opposite one, and it is measured the same way: on the peer's
# own capture, against a live control.
#
# WHAT IS MEASURED, each with a number in the output:
#   1. the emission RATIO — downstream frames out per box frame in, over a 2 s window; the
#      pacing law is one frame per frame, so this is 1.000 or the cadence is invented;
#   2. the frame LENGTH — 1492 B, the fixed downstream, not the box's own geometry;
#   3. the tone's dBFS on the slots wire-format.md puts it on, and silence on the others;
#   4. that a BOX GOING QUIET stops the downstream: a timeout is not a slot, so nothing is
#      emitted for it. Its positive control is the resume, in the same window;
#   5. a head-amp write on the segment, decoded out of the control block of a frame the
#      daemon sent.
#
# ISOLATION IS PART OF THE TEST (tests/hearing-finds-a-segment.sh's header has the full
# reasoning): an unprivileged user+net+pid namespace, a private PipeWire, and the peer end of
# the veth in a NESTED network namespace so the daemon can never hear its own transmissions
# as another host's. The session manager this one additionally needs runs with EVERY hardware
# monitor disabled — the namespace owns no devices, and the ones it can see through /dev are
# the operator's live rig.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw /path/to/fake-box-master}"
FAKE="${2:?usage: $0 /path/to/reac-pw /path/to/fake-box-master}"
SKIP=77

command -v unshare  >/dev/null 2>&1 || { echo "SKIP: no unshare"; exit $SKIP; }
command -v nsenter  >/dev/null 2>&1 || { echo "SKIP: no nsenter"; exit $SKIP; }
command -v python3  >/dev/null 2>&1 || { echo "SKIP: no python3"; exit $SKIP; }
command -v ip       >/dev/null 2>&1 || { echo "SKIP: no iproute2"; exit $SKIP; }
command -v pipewire >/dev/null 2>&1 || { echo "SKIP: no pipewire binary"; exit $SKIP; }
command -v pw-dump  >/dev/null 2>&1 || { echo "SKIP: no pw-dump"; exit $SKIP; }
command -v pw-cli   >/dev/null 2>&1 || { echo "SKIP: no pw-cli"; exit $SKIP; }
command -v pw-cat   >/dev/null 2>&1 || { echo "SKIP: no pw-cat"; exit $SKIP; }
# THE TONE NEEDS SOMETHING TO LINK IT. Without a session manager no node in this graph
# materialises a port, so nothing can be played into the sink and its process() is never
# scheduled — measured, and the reason every earlier veth proof stops at the frame counters.
command -v wireplumber >/dev/null 2>&1 || { echo "SKIP: no wireplumber"; exit $SKIP; }
unshare -r -n -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+pid namespaces unavailable"; exit $SKIP; }

OUT=$(unshare -r -n -p -f --mount-proc --map-root-user bash -s -- "$BIN" "$FAKE" <<'INNER'
set -u
BIN="$1"
FAKE="$2"
LOG=$(mktemp); CONF=$(mktemp -d); RT=$(mktemp -d)
export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
cleanup() { kill -TERM $(jobs -p) 2>/dev/null; sleep 0.3; kill -9 $(jobs -p) 2>/dev/null;
            rm -rf "$LOG" "$CONF" "$RT"; }
trap cleanup EXIT

pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 40); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || {
	echo "SKIP: no private PipeWire in this namespace"; tail -3 "$RT/pw.log"; exit 77; }

# THE SESSION MANAGER, WITH EVERY DEVICE MONITOR OFF. It is here to LINK two nodes that
# already exist, and for nothing else: this namespace has no sound card of its own, and the
# ALSA/V4L2 devices it can reach through /dev belong to the operator's live rig. A drop-in
# beside the shipped config, so the policy that does the linking is the real one.
mkdir -p "$RT/cfg/wireplumber/wireplumber.conf.d"
cat > "$RT/cfg/wireplumber/wireplumber.conf.d/50-no-hardware.conf" <<'WPEOF'
wireplumber.profiles = {
  main = {
    monitor.alsa = disabled
    monitor.alsa-midi = disabled
    monitor.libcamera = disabled
    monitor.v4l2 = disabled
    monitor.bluez = disabled
    monitor.bluez-midi = disabled
    support.reserve-device = disabled
    support.portal-permissionstore = disabled
    # AND IT RESTORES NOTHING ONTO OUR NODES. A new stream is given the session
    # manager's own default volume (0.4 linear, measured), which reac-playback applies
    # to the box's outputs for real — an 8 dB attenuation nobody asked for, sitting
    # between the tone and the number this proof reads.
    stream.restore-props = disabled
    stream.restore-target = disabled
    device.restore-props = disabled
  }
}
WPEOF
XDG_CONFIG_HOME="$RT/cfg" wireplumber >"$RT/wp.log" 2>&1 &
sleep 2

wait_for() {
	local pat="$1" secs="$2" i
	for ((i = 0; i < secs * 5; i++)); do
		grep -q "$pat" "$LOG" && return 0
		sleep 0.2
	done
	return 1
}

# ONE FIELD OUT OF THE EMULATOR'S REPORT. The file is replaced atomically at every write, so
# a read is never of half of one; an ABSENT key prints nothing and every caller below treats
# that as a failure rather than as a zero.
rep() {   # rep <key>
	[ -s "$RT/box.rep" ] || return 1
	awk -v k="$1" '$1 == k { print $2; found = 1 } END { exit !found }' "$RT/box.rep"
}
rep_ch() {   # rep_ch <channel> <rms|peak>
	[ -s "$RT/box.rep" ] || return 1
	awk -v c="ch$1" -v w="$2" '$1 == c { for (i = 2; i < NF; i++) if ($i == w) print $(i+1) }' \
	    "$RT/box.rep"
}
node_id() {   # node_id <node.name>
	pw-dump | python3 -c '
import json,sys
for o in json.load(sys.stdin):
    if o.get("type") == "PipeWire:Interface:Node" and \
       o["info"]["props"].get("node.name") == sys.argv[1]:
        print(o["id"]); break
' "$1"
}

# The peer end is another host: see tests/hearing-finds-a-segment.sh for why a veth with
# both ends here makes every question below unanswerable.
unshare -n sleep 900 &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n true 2>/dev/null || {
	echo "SKIP: no nested network namespace for the peer end"; exit 77; }
peer() { nsenter -t $NSPID -n "$@"; }
in_peer="nsenter -t $NSPID -n"

ip link add bmx0 type veth peer name mbx0 || exit 90
ip link set mbx0 netns $NSPID || exit 90

BOXMAC=00:40:ab:c4:08:bc
# The box is transmitting before the carrier exists, so the wire carries a box master from
# the first instant of link and the masterless licence is never in the race.
$in_peer "$FAKE" mbx0 "$BOXMAC" 8 2000 "$RT/box.rep" >"$RT/box.log" 2>&1 &
FAKEPID=$!
sleep 0.5
ip link set bmx0 up; peer ip link set mbx0 up

HOME="$CONF" REAC_DEBUG=1 "$BIN" >"$LOG" 2>&1 &
PID=$!

wait_for "\[bmx0\] box masters this wire" 20 || {
	echo "FAIL: a box mastered the wire and the daemon never joined it"
	tail -20 "$LOG"; tail -3 "$RT/box.log"; exit 1; }
wait_for "\[bmx0\] segment up" 20 || {
	echo "FAIL: joined, but the segment never came up"; tail -20 "$LOG"; exit 1; }

# ---- 1. THE SEGMENT HAS AN OUTPUT, SIZED BY THE BOX. -------------------------------
sleep 2
PLAY=$(node_id reac-playback.bmx0)
[ -n "$PLAY" ] || {
	echo "FAIL: a joined box master published no reac-playback.bmx0 — its outputs are"
	echo "      unroutable, which is the 0.5.1 contract 0.5.5 overturns"
	pw-dump | grep -o '"node.name": "[^"]*"' | sort -u | head; tail -20 "$LOG"; exit 1; }
NPORT=$(pw-dump | python3 -c '
import json,sys
d = json.load(sys.stdin); want = int(sys.argv[1]); n = 0
for o in d:
    if o.get("type") == "PipeWire:Interface:Port":
        p = o["info"]["props"]
        if int(p.get("node.id", -1)) == want and p.get("port.direction") == "in":
            n += 1
print(n)' "$PLAY")
[ "$NPORT" = "8" ] || {
	echo "FAIL: reac-playback.bmx0 has $NPORT input ports; an S-0808 has 8 outputs"; exit 1; }

# ---- 2. THE RATIO: ONE DOWNSTREAM FRAME PER BOX FRAME. -----------------------------
# Sampled over a window, not since the start: the box was transmitting before the daemon
# ever saw the wire, and the frames from before the join are not this measure's business.
TX1=$(rep tx) && RX1=$(rep rx_down) || {
	echo "FAIL: the emulator wrote no report at all — nothing below can be measured"
	tail -5 "$RT/box.log"; exit 1; }
sleep 2
TX2=$(rep tx); RX2=$(rep rx_down)
DTX=$((TX2 - TX1)); DRX=$((RX2 - RX1))
# THE PROBE'S OWN POSITIVE CONTROL, before any claim about the ratio: a capture that
# receives nothing reports a ratio of 0.000 and a capture that is not running reports the
# same, and neither is a measurement of the daemon.
[ "$DTX" -gt 1000 ] || {
	echo "FAIL: the box emulator sent only $DTX frames in 2 s — its own transmit is what"
	echo "      the ratio is measured against, so nothing below would mean anything"; exit 1; }
[ "$DRX" -gt 0 ] || {
	echo "FAIL: the daemon joined a box master and sent NOTHING back over 2 s ($DRX frames"
	echo "      of $DTX). Since 0.5.5 the downstream is sent on this wire"
	cat "$RT/box.rep"; grep -E "bmx0|pacer" "$LOG" | tail -10; exit 1; }
RATIO=$(python3 -c "print('%.4f' % ($DRX / $DTX))")
python3 -c "import sys; sys.exit(0 if abs($DRX/$DTX - 1.0) <= 0.01 else 1)" || {
	echo "FAIL: the emission ratio is $RATIO downstream frames per box frame ($DRX/$DTX)."
	echo "      The pacing law is one frame per frame: the box's arrival IS the slot."
	cat "$RT/box.rep"; exit 1; }
echo "MEASURED: emission ratio $RATIO downstream frames per box frame ($DRX/$DTX over 2 s)"

# ---- 3. THE FRAME IS THE FIXED DOWNSTREAM, AND NOTHING PRECEDED THE BOX. -----------
LEN=$(rep last_len)
[ "$LEN" = "1492" ] || {
	echo "FAIL: the daemon sent a $LEN B frame; a master downstream is 1492 B at every"
	echo "      rate and every box width (wire-format.md)"; exit 1; }
BEFORE=$(rep rx_before_tx)
[ "$BEFORE" = "0" ] || {
	echo "FAIL: $BEFORE downstream frames reached the box before it had sent one. Nothing"
	echo "      may leave until the box's first frame — the tick IS that frame"; exit 1; }
BAD=$(rep rx_bad_decode)
[ "$BAD" = "0" ] || {
	echo "FAIL: $BAD of the daemon's frames would not decode as a REAC downstream"; exit 1; }
echo "MEASURED: frame length ${LEN} B, ${BEFORE} frames sent before the box's first, ${BAD} undecodable"

# ---- 4. THE TONE: THE OPERATOR'S JOB, END TO END. ----------------------------------
# A 1 kHz sine is played into the segment's playback node and read back off the wire at the
# far end, decoded by libreac. It is played TWICE, 20 dB apart, and what is asserted is the
# DELTA: a graph has gain staging in it that this proof does not own — measured here, the
# session manager alone puts 8 dB between a player's full scale and a node's input — and an
# absolute reading would be asserting that stage rather than the daemon's. A ratio survives
# a constant nobody declared; an absolute does not. Both absolutes are still REPORTED, and
# the loud one is required to be a SIGNAL rather than a number near the floor, or the delta
# would be two silences agreeing with each other.
play_tone() {   # play_tone <amplitude> -> echoes "<ch0-rms> <ch1-rms> <ch0-peak>"
	python3 - "$RT/tone.wav" "$1" <<'PYEOF'
import math, struct, sys, wave
amp = float(sys.argv[2])
w = wave.open(sys.argv[1], "wb")
w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b"".join(struct.pack("<hh", *(2 * (int(amp * 32767 * math.sin(2 * math.pi * 1000 * n / 48000)),)))
                       for n in range(48000 * 6)))
w.close()
PYEOF
	pw-cat --playback --volume 1.0 --target reac-playback.bmx0 "$RT/tone.wav" \
	       >"$RT/cat.log" 2>&1 &
	CATPID=$!
	sleep 2.5
	LINKED=$(pw-link -l 2>/dev/null | grep -c "reac-playback.bmx0")
	if [ "$LINKED" -eq 0 ]; then
		echo "UNLINKED"
		kill -TERM $CATPID 2>/dev/null
		return
	fi
	sleep 1
	echo "$(rep_ch 0 rms) $(rep_ch 1 rms) $(rep_ch 0 peak) $(rep_ch 5 rms)"
	kill -TERM $CATPID 2>/dev/null; wait $CATPID 2>/dev/null
	sleep 0.5
}

LOUD=$(play_tone 0.5)
[ "$LOUD" != "UNLINKED" ] || {
	echo "FAIL: the tone player never linked to reac-playback.bmx0, so no audio was ever"
	echo "      offered to the segment and its silence would prove nothing"
	pw-link -l; tail -5 "$RT/cat.log"; tail -5 "$RT/wp.log"; exit 1; }
set -- $LOUD; L0="$1"; L1="$2"; LPK="$3"; LQ="$4"
SOFT=$(play_tone 0.05)
[ "$SOFT" != "UNLINKED" ] || { echo "FAIL: the second tone never linked"; exit 1; }
set -- $SOFT; S0="$1"; S1="$2"

[ -n "$L0" ] && [ -n "$L1" ] && [ -n "$S0" ] && [ -n "$LQ" ] || {
	echo "FAIL: the emulator reported no per-channel energy at all"; cat "$RT/box.rep"; exit 1; }
# THE BASELINE IS A SIGNAL. Two readings 20 dB apart at the bottom of the floor would pass
# a delta test and mean nothing: -110 -> -130 is not a gain measurement (CLAUDE.md's own
# lesson, 2026-08-13). The loud tone must be well clear of the floor first.
python3 -c "import sys; sys.exit(0 if $L0 > -40.0 and $L1 > -40.0 else 1)" || {
	echo "FAIL: a 0.5 FS sine was played into the segment and slots 0/1 of the downstream"
	echo "      carry $L0 / $L1 dBFS — that is the floor, not audio"
	cat "$RT/box.rep"; exit 1; }
# AND THE SLOTS NOBODY FED ARE SILENT. Placement is half the claim: the right samples in
# the wrong slots is a downstream that decodes and plays the wrong thing.
python3 -c "import sys; sys.exit(0 if $LQ < -60.0 else 1)" || {
	echo "FAIL: slot 5 was fed nothing and carries $LQ dBFS — the placement is wrong, or"
	echo "      the frame is being filled with something that is not the graph's audio"
	cat "$RT/box.rep"; exit 1; }
DELTA=$(python3 -c "print('%.2f' % ($L0 - $S0))")
python3 -c "import sys; sys.exit(0 if abs(($L0) - ($S0) - 20.0) < 1.5 else 1)" || {
	echo "FAIL: a 20 dB change at the sink moved the wire by $DELTA dB. The downstream is"
	echo "      not carrying the graph's audio linearly"; exit 1; }
echo "MEASURED: tone at slots 0/1 = $L0 / $L1 dBFS RMS (peak $LPK), unfed slot 5 = $LQ dBFS;"
echo "          -20 dB at the source reads $S0 dBFS, a delta of $DELTA dB"

# ---- 5. A HEAD-AMP WRITE REACHES THE WIRE, THROUGH THE PUBLISHED PATH. -------------
# The console does not know a wire channel. It reads reac.headamp.channels / .base off the
# node and composes the key for INPUT 1 as base + 1 - 1; a `channels` of 0 means "this box
# has no preamps" and its gain, pad and phantom writes are refused before they are sent.
# That is what the rig read on a joined box master (2026-09-09, S-0808 at 0 against the
# S-1608 on the neighbouring segment at 16) — so the capabilities are read HERE, from the
# node, and the write is composed from them exactly as the console composes it.
HA_CH=$(pw-dump | python3 -c '
import json,sys
for o in json.load(sys.stdin):
    if o.get("type") == "PipeWire:Interface:Node" and \
       o["info"]["props"].get("node.name") == "reac-playback.bmx0":
        p = o["info"]["props"]
        print(p.get("reac.headamp.channels","(none)"), p.get("reac.headamp.base","(none)"),
              p.get("reac.headamp.caps","(none)"))
        break')
set -- $HA_CH; HACH="${1:-}"; HABASE="${2:-}"; HACAPS="${3:-}"
[ "$HACH" = "8" ] || {
	echo "FAIL: reac-playback.bmx0 publishes reac.headamp.channels=$HACH. An S-0808 has 8"
	echo "      preamps and its width is on the wire; a console reads 0 as 'no preamps'"
	echo "      and never sends the write at all"; exit 1; }
# The S-0808's chassis strap is 0 and the S-1608's is 32: the base is READ from the model
# row's declaration byte, never computed from the width, so this pins the row and not a
# formula that happens to agree at one width.
[ "$HABASE" = "0" ] || {
	echo "FAIL: reac.headamp.base=$HABASE; the S-0808 declares strap byte 0, so base 0"; exit 1; }
case "$HACAPS" in
  *phantom*|*sens*) : ;;
  *) echo "FAIL: reac.headamp.caps=$HACAPS names no capability"; exit 1 ;;
esac
# COMPOSED THE WAY A CONSOLE COMPOSES IT: input 1 of this box is wire channel base + 0.
WIRECH=$((HABASE + 0))
pw-cli set-param "$PLAY" Props \
	"{ params = [ \"reac.headamp.$WIRECH.sens\", 20 ] }" >/dev/null 2>&1
for i in $(seq 40); do
	HA=$(awk '$1 == "headamp" { print $3, $5, $7 }' "$RT/box.rep" 2>/dev/null)
	[ -n "$HA" ] && break
	sleep 0.25
done
[ -n "$HA" ] || {
	echo "FAIL: a head-amp write composed from this node's own published capabilities never"
	echo "      appeared in any control block the daemon sent"
	cat "$RT/box.rep"; exit 1; }
set -- $HA
[ "$1" = "$WIRECH" ] && [ "$2" = "2" ] && [ "$3" = "20" ] || {
	echo "FAIL: the head-amp record on the wire reads ch=$1 param=$2 value=$3; the write was"
	echo "      wire channel $WIRECH, sens (param 2), value 20"; exit 1; }
echo "MEASURED: head-amp published as channels=$HACH base=$HABASE caps=$HACAPS; a write on"
echo "          input 1 (wire ch $WIRECH) arrives as ch $1, param $2 (sens), value $3"

# ---- 6. THE BOX GOES QUIET AND THE DOWNSTREAM STOPS WITH IT. -----------------------
# A timeout is not a slot. This is the same law as "nothing before the first frame", in the
# direction a running wire can actually be asked about.
kill -USR1 $FAKEPID
sleep 0.8
Q1=$(rep rx_down); sleep 1.5; Q2=$(rep rx_down)
QD=$((Q2 - Q1))
[ "$QD" -le 20 ] || {
	echo "FAIL: the box stopped transmitting and the daemon put $QD more frames on the wire"
	echo "      over 1.5 s. With no frame there is no slot, so there is nothing to send"
	exit 1; }
# ITS POSITIVE CONTROL: the same counter, over the same length of window, with the box back.
kill -USR1 $FAKEPID
sleep 0.8
R1=$(rep rx_down); sleep 1.5; R2=$(rep rx_down)
RD=$((R2 - R1))
[ "$RD" -gt 1000 ] || {
	echo "FAIL: the box resumed and the daemon sent only $RD frames in 1.5 s, so the quiet"
	echo "      window above measured a dead daemon rather than an obeyed cadence"; exit 1; }
echo "MEASURED: box quiet -> $QD frames sent in 1.5 s; box back -> $RD frames in 1.5 s"

kill -TERM $FAKEPID 2>/dev/null; wait $FAKEPID 2>/dev/null
kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null
echo "PASS: a box master is joined AND driven — one downstream per box frame, audio placed, head-amp delivered"
INNER
)
rc=$?
echo "$OUT"
[ $rc -eq 77 ] && exit 77
[ $rc -ne 0 ] && exit $rc
echo "$OUT" | grep -q "^PASS:" || { echo "FAIL: the inner namespace produced no verdict"; exit 1; }
exit 0
