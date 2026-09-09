#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY, END TO END: a stagebox on M masters a wire, and the daemon ENROLS WITH IT
# THE WAY A STAGEBOX DOES — then routes audio to its outputs.
#
# THE RULING (2026-09-09): "It is only a matter of following the same protocol that we
# expect." 0.5.5 sent a DESK'S downstream at a box on M and the box ignored every byte of
# it; the ground-truth capture of a real S-1608 meeting the real S-0808
# (DESIGN.md 0.5.6) shows what that box actually grants: a SLAVE speaking the MASTER'S OWN
# geometry. This proof is that recipe, measured at the far end of the cable:
#
#   1. the flood is broadcast, at the MASTER's width (340 B / 8 slots), and bounded;
#   2. the config-announce comes FIRST, as the frame the daemon goes unicast with, and the
#      cold-connect burst follows it — the ORDER is the assertion, and its sabotage is
#      reversing it;
#   3. the steady state is unicast at the master's width carrying the reac-playback sink's
#      channels — a tone is played into that node and decoded off the emulator (slot, dBFS);
#   4. the heartbeat cadence;
#   5. and none of it happens before the box has spoken.
#
# ISOLATION IS PART OF THE TEST (tests/hearing-finds-a-segment.sh's header has the full
# reasoning): an unprivileged user+net+pid namespace, a private PipeWire, the peer end of the
# veth in a NESTED network namespace. The session manager the tone needs runs with EVERY
# hardware monitor disabled — the namespace owns no devices and the ones it can see through
# /dev are the operator's live rig.
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

export REACPW_BOX_MASTER_FRAME="${REACPW_BOX_MASTER_FRAME:-mixer}"
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
# field N of a keyed line in the emulator's report ("up frames 15817 len 340 ch 8 ...")
rep_f() {   # rep_f <key> <field-index> <file>
	[ -s "$3" ] || return 1
	awk -v k="$1" -v n="$2" '$1 == k { print $n; f = 1; exit } END { exit !f }' "$3"
}
# one slot's energy on the UPSTREAM the daemon sends
up_ch() {   # up_ch <slot> <rms|peak>
	[ -s "$RT/box.rep" ] || return 1
	awk -v c="$1" -v w="$2" '$1 == "upch" && $2 == c { for (i = 3; i < NF; i++) if ($i == w) print $(i+1) }' \
	    "$RT/box.rep"
}
rep_ch() {   # rep_ch <channel> <rms|peak>
	[ -s "$RT/box.rep" ] || return 1
	awk -v c="ch$1" -v w="$2" '$1 == c { for (i = 2; i < NF; i++) if ($i == w) print $(i+1) }' \
	    "$RT/box.rep"
}
# ONE PROPERTY of one of this daemon's nodes, empty when the node is not there.
node_prop() {   # node_prop <node.name> <key>
	pw-dump | python3 -c '
import json,sys
for o in json.load(sys.stdin):
    if o.get("type") == "PipeWire:Interface:Node" and \
       o["info"]["props"].get("node.name") == sys.argv[1]:
        print(o["info"]["props"].get(sys.argv[2], "")); break
' "$1" "$2"
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

HOME="$CONF" REAC_DEBUG=1 REACPW_BOX_MASTER_FRAME="${REACPW_BOX_MASTER_FRAME:-mixer}" \
	"$BIN" >"$LOG" 2>&1 &
PID=$!

wait_for "\[bmx0\] box masters this wire" 20 || {
	echo "FAIL: a box mastered the wire and the daemon never joined it"
	tail -20 "$LOG"; tail -3 "$RT/box.log"; exit 1; }
wait_for "\[bmx0\] segment up" 20 || {
	echo "FAIL: joined, but the segment never came up"; tail -20 "$LOG"; exit 1; }

# ---- 1. THE FLOOD: BROADCAST, AT THE MASTER'S WIDTH, BOUNDED. ----------------------
wait_for "\[bmx0\] SLAVE role on a BOX MASTER" 25 || {
	echo "FAIL: a box mastered the wire and the daemon did not enrol with it as a slave"
	tail -20 "$LOG"; tail -3 "$RT/box.log"; exit 1; }
for i in $(seq 120); do
	FL=$(rep_f flood 3 "$RT/box.rep"); [ -n "$FL" ] && [ "$FL" -gt 5000 ] && break
	sleep 0.5
done
FL=$(rep_f flood 3 "$RT/box.rep"); FLEN=$(rep_f flood 5 "$RT/box.rep")
[ -n "$FL" ] && [ "$FL" -gt 5000 ] || {
	echo "FAIL: the daemon broadcast only ${FL:-0} flood frames; a box announces itself"
	echo "      with a bounded flood before it may go unicast"; cat "$RT/box.rep"; exit 1; }
# THE GEOMETRY IS THE EXPERIMENT (0.5.6-3). Default is the operator's ruling — a mixer sends
# 40 channels, so every audio frame is the fixed 1492 B downstream, in the flood as much as
# after the grant. REACPW_BOX_MASTER_FRAME=box is the other corner the two rig runs left
# open: an exact S-1608 imitation at 340 B. The proof measures whichever it was told to run,
# and asserts the geometry it asked for rather than a constant.
WANT_LEN=1492; WANT_WHAT="the mixer's 40 slots"
[ "${REACPW_BOX_MASTER_FRAME:-mixer}" = "box" ] && { WANT_LEN=340; WANT_WHAT="the box's own 8 slots"; }
[ "$FLEN" = "$WANT_LEN" ] || {
	echo "FAIL: the flood frames are $FLEN B; REACPW_BOX_MASTER_FRAME=${REACPW_BOX_MASTER_FRAME:-mixer}"
	echo "      asks for $WANT_LEN B ($WANT_WHAT)"; exit 1; }
echo "MEASURED: flood $FL frames of $FLEN B ($WANT_WHAT), broadcast"

# ---- 2. THE ORDER: ANNOUNCE FIRST, THEN THE BURST. ---------------------------------
# This is the assertion the ground-truth capture is FOR. The S-1608 went unicast WITH its
# config-announce and sent the cold-connect burst 214 ms later; reversing the two is the
# sabotage this phase exists to catch.
for i in $(seq 80); do
	AN=$(rep_f up 9 "$RT/box.rep"); JN=$(rep_f up 11 "$RT/box.rep")
	[ -n "$JN" ] && [ "$JN" -ge 3 ] && break
	sleep 0.25
done
AN=$(rep_f up 9 "$RT/box.rep"); JN=$(rep_f up 11 "$RT/box.rep")
ULEN=$(rep_f up 5 "$RT/box.rep")
UCH=$(awk '$1 == "up" { for (i = 2; i < NF; i++) if ($i == "ch") print $(i+1) }' "$RT/box.rep")
[ -n "$AN" ] && [ "$AN" -ge 1 ] || {
	echo "FAIL: no config-announce ever reached the box master"; cat "$RT/box.rep"; exit 1; }
[ -n "$JN" ] && [ "$JN" -ge 3 ] || {
	echo "FAIL: the cold-connect burst never reached the box master (${JN:-0} records; the"
	echo "      capture shows three)"; cat "$RT/box.rep"; exit 1; }
ORD=$(awk '$1 == "up" && $2 == "order" { print $4 }' "$RT/box.rep")
[ -n "$ORD" ] || { echo "FAIL: the emulator saw no announce/burst ordering at all"; exit 1; }
python3 -c "import sys; sys.exit(0 if $ORD > 0 else 1)" || {
	echo "FAIL: the cold-connect burst arrived $ORD s BEFORE the config-announce. A box"
	echo "      announces itself first and cold-connects after — the master enrols it from"
	echo "      that announce"; exit 1; }
[ "$ULEN" = "$WANT_LEN" ] || {
	echo "FAIL: the unicast control frames are $ULEN B; they ride the same carrier as the"
	echo "      audio, which this run asked to be $WANT_LEN B"; exit 1; }
# THE DECLARATION IS THE ONE THAT WAS GRANTED, byte for byte. 0.5.6-1 derived it from the
# MASTER's width and announced selector 0x84 — the family of the box it was talking TO — and
# the real S-0808 echoed nothing behind four correct bursts. This is the block a real S-1608
# unicast to that same chassis 4 ms before it was granted.
# OUR OWN INVENTORY, not the peer's. Both real boxes announce selector 0x80 with their own
# port table — an 8-input box `01 01 01 01 02 02`, a 16-input one `02 02 02 02 01 01` — and
# each was granted by the other. Announcing the S-1608's table to an S-1608 master told it
# its own identity back and was refused twice. The emulator here is 8-out, so we fill eight
# slots and declare the eight-input inventory.
S1608_ANN=cdea0103001080000000010101010202030303030303000000000000000000000052
ANNBLK=$(awk '$1 == "announceblk" { print $2 }' "$RT/box.rep")
[ "$ANNBLK" = "$S1608_ANN" ] || {
	echo "FAIL: our config-announce is not the one that was granted."
	echo "      ours   $ANNBLK"
	echo "      S-1608 $S1608_ANN"
	python3 - "$ANNBLK" "$S1608_ANN" <<'PYEOF'
import sys
a = bytes.fromhex(sys.argv[1]); b = bytes.fromhex(sys.argv[2])
print("      differs at offsets:", [i for i in range(min(len(a), len(b))) if a[i] != b[i]])
PYEOF
	exit 1; }
AOK=$(awk '$1 == "announce" && $2 == "ok" { print $3 }' "$RT/box.rep")
[ "$AOK" = "1" ] || {
	echo "FAIL: the box refused our declaration ($(awk '$1=="announce"&&$2=="ok"{print $5}' "$RT/box.rep") refusals)"
	exit 1; }
# THREE DISTINCT RECORDS. The box echoes one per distinct record — three for the S-1608's
# three, two when its second is a copy of its first — so a burst that repeats a record asks
# for a two-record answer where a real box asks for three.
DIST=$(awk '$1 == "distinct" && $2 == "records" { print $3 }' "$RT/box.rep")
[ "$DIST" = "3" ] || {
	echo "FAIL: the box master saw $DIST DISTINCT cold-connect records; the granted S-1608"
	echo "      sent three (0014, a different 0014, then 0013)"; cat "$RT/box.rep"; exit 1; }
echo "MEASURED: $DIST distinct cold-connect records, as the granted box sent"
echo "MEASURED: announce then burst, $AN announce / $JN cold-connect records, the burst"
echo "          $ORD s after the announce; control frames $ULEN B, unicast; the announce"
echo "          block is byte-identical to the S-1608's (selector 0x80 at offset 6)"

# ---- 3. IT ESTABLISHED, AND IT HEARTBEATS. -----------------------------------------
wait_for "reac_slave: .*STATE .*-> ESTABLISHED" 30 || {
	echo "FAIL: the box master granted and the engine never reached ESTABLISHED"
	grep "reac_slave: .*STATE" "$LOG" | tail -6; cat "$RT/box.rep"; exit 1; }
for i in $(seq 60); do
	HB=$(rep_f up 13 "$RT/box.rep"); [ -n "$HB" ] && [ "$HB" -ge 3 ] && break
	sleep 0.5
done
HB=$(rep_f up 13 "$RT/box.rep")
HBP=$(awk '$1 == "up" && $2 == "hb_period" { print $3 }' "$RT/box.rep")
[ -n "$HB" ] && [ "$HB" -ge 3 ] || {
	echo "FAIL: only ${HB:-0} heartbeats reached the box master; a linked box beats ~1/s"
	cat "$RT/box.rep"; exit 1; }
# THE PERIOD IS FRAME-COUNTED, so on a veth whose emulator paces slower than the rate the
# daemon recovered from it the wall-clock period stretches by exactly that ratio. What is
# asserted is that it BEATS on a sane cadence; the 1.00 s the ground truth measured belongs
# to a wire actually running at its nominal rate.
python3 -c "import sys; sys.exit(0 if 0.3 < $HBP < 4.0 else 1)" || {
	echo "FAIL: the heartbeat period is $HBP s — a linked box beats about once a second"
	exit 1; }
# THE STATE CLAIM AND WHEN IT WAS MADE. The rig sent the ESTABLISHED descriptor (007a) from
# its first unicast frame and was never granted; the box that WAS granted sent zeros there
# until after its grant. The emulator refuses a pre-grant descriptor now, so this reads the
# frame index of each and requires the order.
DESC=$(awk '$1 == "descriptor" { print $3 }' "$RT/box.rep")
GRF=$(awk '$1 == "descriptor" { print $5 }' "$RT/box.rep")
DBG=$(awk '$1 == "descriptor" { print $7 }' "$RT/box.rep")
[ "$DBG" = "0" ] || {
	echo "FAIL: the ESTABLISHED descriptor was on the wire before the grant — we told the"
	echo "      box we were linked to it before it granted anything"; exit 1; }
[ -n "$DESC" ] && [ "$DESC" != "0" ] && [ "$DESC" -gt "${GRF:-0}" ] || {
	echo "FAIL: the descriptor never appeared after the grant (first='$DESC' grant='$GRF'),"
	echo "      so an established peer is indistinguishable from a joining one"; exit 1; }
# AND THE REQUESTING STATE BEFORE IT. A real slave's fillers carry 0x52 from its announce
# until the grant; zeroing that window is REFUSED by a real S-1608 (replay V9e), and ours
# carried zero for the whole enrolment — the only field-level difference across the two
# streams.
REQ=$(awk '$1 == "descriptor" { print $9 }' "$RT/box.rep")
[ -n "$REQ" ] && [ "$REQ" -gt 100 ] || {
	echo "FAIL: only ${REQ:-0} fillers carried the REQUESTING descriptor between our announce"
	echo "      and the grant; a real slave fills that whole window with it"; exit 1; }
echo "MEASURED: $REQ fillers carried REQUESTING (0x52) between the announce and the grant"
echo "MEASURED: ESTABLISHED descriptor first at peer frame $DESC, grant at $GRF — after, as"
echo "          the granted box sent it"
echo "MEASURED: established; $HB heartbeats, period $HBP s (frame-counted: this emulator"
echo "          paces slower than the rate the daemon recovered, and it scales with that)"

# ---- 4. THE TONE: THE OPERATOR'S JOB, INTO THE BOX MASTER'S OUTPUTS. ---------------
PLAY=$(node_id reac-playback.bmx0)
[ -n "$PLAY" ] || {
	echo "FAIL: the segment published no reac-playback.bmx0, so the box master's outputs"
	echo "      are unroutable"; pw-dump | grep -o '"node.name": "[^"]*"' | sort -u | head
	exit 1; }
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
	echo "FAIL: reac-playback.bmx0 has $NPORT input ports; the box master declared 8"; exit 1; }
# Played TWICE, 20 dB apart: a graph has gain staging this proof does not own (the session
# manager alone puts 8 dB between a player's full scale and a node's input, measured), so
# what is asserted is the DELTA. A ratio survives a constant nobody declared.
play_tone() {   # play_tone <amplitude> -> "<ch0-rms> <ch1-rms> <ch0-peak> <ch5-rms>"
	python3 - "$RT/tone.wav" "$1" <<'PYEOF'
import math, struct, sys, wave
amp = float(sys.argv[2])
w = wave.open(sys.argv[1], "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b"".join(struct.pack("<hh", *(2 * (int(amp * 32767 * math.sin(2 * math.pi * 1000 * n / 48000)),)))
                       for n in range(48000 * 6)))
w.close()
PYEOF
	pw-cat --playback --volume 1.0 --target reac-playback.bmx0 "$RT/tone.wav" \
	       >"$RT/cat.log" 2>&1 &
	CATPID=$!
	sleep 2.5
	if [ "$(pw-link -l 2>/dev/null | grep -c reac-playback.bmx0)" -eq 0 ]; then
		echo "UNLINKED"; kill -TERM $CATPID 2>/dev/null; return
	fi
	sleep 1
	if [ "$WANT_LEN" = "340" ]; then
		echo "$(up_ch 0 rms) $(up_ch 1 rms) $(up_ch 0 peak) $(up_ch 5 rms)"
	else
		echo "$(rep_ch 0 rms) $(rep_ch 1 rms) $(rep_ch 0 peak) $(rep_ch 5 rms)"
	fi
	kill -TERM $CATPID 2>/dev/null; wait $CATPID 2>/dev/null; sleep 0.5
}
LOUD=$(play_tone 0.5)
[ "$LOUD" != "UNLINKED" ] || {
	echo "FAIL: the tone player never linked to reac-playback.bmx0, so no audio was ever"
	echo "      offered and its silence would prove nothing"
	pw-link -l; tail -5 "$RT/cat.log"; tail -5 "$RT/wp.log"; exit 1; }
set -- $LOUD; L0="$1"; L1="$2"; LPK="$3"; LQ="$4"
SOFT=$(play_tone 0.05)
[ "$SOFT" != "UNLINKED" ] || { echo "FAIL: the second tone never linked"; exit 1; }
set -- $SOFT; S0="$1"
[ -n "$L0" ] && [ -n "$S0" ] && [ -n "$LQ" ] || {
	echo "FAIL: the emulator reported no per-slot energy on the upstream"
	cat "$RT/box.rep"; exit 1; }
python3 -c "import sys; sys.exit(0 if $L0 > -40.0 and $L1 > -40.0 else 1)" || {
	echo "FAIL: a 0.5 FS sine was played into the segment and the downstream's slots 0/1 carry"
	echo "      $L0 / $L1 dBFS — that is the floor, not audio. The box master's outputs are"
	echo "      fed by THIS stream"; cat "$RT/box.rep"; exit 1; }
python3 -c "import sys; sys.exit(0 if $LQ < -60.0 else 1)" || {
	echo "FAIL: slot 5 was fed nothing and carries $LQ dBFS — wrong placement"; exit 1; }
DELTA=$(python3 -c "print('%.2f' % ($L0 - $S0))")
python3 -c "import sys; sys.exit(0 if abs(($L0) - ($S0) - 20.0) < 1.5 else 1)" || {
	echo "FAIL: a 20 dB change at the sink moved the wire by $DELTA dB"; exit 1; }
echo "MEASURED: tone in the $WANT_WHAT at slots 0/1 = $L0 / $L1 dBFS RMS (peak $LPK), unfed"
echo "          slot 5 = $LQ dBFS; -20 dB at the source reads $S0, a delta of $DELTA dB"

# ---- 5. AND NOTHING WAS SENT BEFORE THE BOX SPOKE. ---------------------------------
BEFORE=$(rep rx_before_tx)
[ "$BEFORE" = "0" ] || {
	echo "FAIL: $BEFORE frames reached the box before it had sent one"; exit 1; }
# THE ABSENCE CLAIM'S POSITIVE CONTROL: the same counter, for frames that DID arrive.
SB=$(awk '$1 == "steady" && $2 == "bcast" { print $3 }' "$RT/box.rep")
UF=$(rep_f up 3 "$RT/box.rep")
# In box geometry the steady state is UNICAST, as the granted box sent it, so the control
# that makes the absence above mean something is whichever counter this run fills.
[ "$(( ${SB:-0} + ${UF:-0} ))" -gt 1000 ] || {
	echo "FAIL: too few steady-state frames for that absence to mean anything"; exit 1; }
echo "MEASURED: 0 frames before the box's first; $SB broadcast and $UF unicast frames after"
echo "          the announce (REACPW_BOX_MASTER_FRAME=${REACPW_BOX_MASTER_FRAME:-mixer})"

# ---- 6. THE LAMP IS THE PAIRING, NOT THE HEARING. ---------------------------------
# The rig, 2026-09-09: "S-0808 is not enrolled but omx sees it available" — the segment
# published reac.link-state=established off the RX's own evidence while the box's front
# lamp sat unlocked. A console keys a stagebox off that key (openmixer's
# stageboxConnected: only `established` is a locked, streaming box), so it must follow the
# ENGINE. The timeline below is that claim, both edges of it, on the graph.
LS=$(node_prop reac-capture.bmx0 reac.link-state)
[ "$LS" = "established" ] || {
	echo "FAIL: the segment is enrolled and streaming and publishes link-state=$LS"; exit 1; }
# BOTH DOORS, THE SAME ANSWER. A console folds the two nodes of a segment into one row, so a
# capture reading `established` beside a playback still at `probing` reads as a resync in
# progress — which is what the rig showed the moment the engine enrolled, because on this
# path the sink runs with no pacer and so no badge timer to move it.
PLS=$(node_prop reac-playback.bmx0 reac.link-state)
[ "$PLS" = "$LS" ] || {
	echo "FAIL: the segment's two doors disagree — reac-capture says '$LS' and"
	echo "      reac-playback says '$PLS'"; exit 1; }
PBW=$(node_prop reac-playback.bmx0 reac.box-width)
CBW=$(node_prop reac-capture.bmx0 reac.box-width)
[ "$PBW" = "$CBW" ] || {
	echo "FAIL: the two doors disagree about the box's width ('$CBW' / '$PBW')"; exit 1; }
echo "MEASURED: both doors read link-state=$LS, box-width=$CBW"
# The drop edge: the wire goes away past the hold, and the lamp must go back.
ip link set bmx0 down
T0=$(date +%s.%N)
for i in $(seq 80); do
	LS=$(node_prop reac-capture.bmx0 reac.link-state)
	[ -z "$LS" ] || [ "$LS" = "probing" ] && break
	sleep 0.25
done
T1=$(date +%s.%N)
[ -z "$LS" ] || [ "$LS" = "probing" ] || {
	echo "FAIL: the link went down and the segment still publishes link-state=$LS"; exit 1; }
echo "MEASURED: link-state established -> ${LS:-<segment gone>} $(python3 -c "print('%.1f' % ($T1 - $T0))") s after the link went down"
# And back up. THE WHOLE POINT IS THE WINDOW IN BETWEEN: the segment comes back, its
# capture door is real and streaming, and it must read `probing` for as long as the recipe
# is still running — flood, announce, burst, waiting for the grant echo. Reading
# `established` there is the rig's own defect, and it is what a console renders as a locked
# stagebox that is not locked.
ip link set bmx0 up
T2=$(date +%s.%N)
SAW_PROBING=0
for i in $(seq 200); do
	LS=$(node_prop reac-capture.bmx0 reac.link-state)
	[ "$LS" = "probing" ] && SAW_PROBING=1
	[ "$LS" = "established" ] && break
	sleep 0.1
done
T3=$(date +%s.%N)
[ "$SAW_PROBING" = "1" ] || {
	echo "FAIL: the segment went straight to established without ever publishing probing."
	echo "      The enrolment takes seconds; a console must not be told the box is locked"
	echo "      while the flood and the cold-connect are still running"; exit 1; }
echo "MEASURED: link-state probing while the recipe ran, on the segment's own live door"
[ "$LS" = "established" ] || {
	echo "FAIL: the wire came back and the segment never re-enrolled (link-state=$LS)"
	grep "reac_slave: .*STATE" "$LOG" | tail -6; exit 1; }
echo "MEASURED: re-enrolled, link-state established $(python3 -c "print('%.1f' % ($T3 - $T2))") s after the link returned"
# THE ABSENCE CLAIM'S POSITIVE CONTROL: `probing` must be a state this probe can SEE, not
# just one it failed to read. It was read above, on the drop edge, from the same node.

# ---- 7. A 16-INPUT BOX HAS EIGHT OUTPUTS, AND BOTH DOORS SAY SO. -------------------
# The rig, with the S-1608 in master mode: reac-playback.<segment> came up at SIXTEEN
# channels, because the door was sized from the width the box BROADCASTS - which is its input
# count - instead of the model row's output count. An S-0808 hid it (8 in, 8 out); an S-1608
# does not. Both real captures agree the frames carry 8 slots to an 8-out master. And the
# capture door read the generic "REAC 16ch capture" where the master path's names the box,
# which is what a console shows the operator.
ip link add bmx1 type veth peer name mbx1 || exit 90
ip link set mbx1 netns $NSPID || exit 90
$in_peer "$FAKE" mbx1 00:40:ab:c4:80:41 16 2000 "$RT/box16.rep" >"$RT/box16.log" 2>&1 &
FAKE16=$!
sleep 0.5
ip link set bmx1 up; peer ip link set mbx1 up
wait_for "\[bmx1\] SLAVE role on a BOX MASTER" 30 || {
	echo "FAIL: a 16-ch box master was not joined"; tail -10 "$LOG"; exit 1; }
sleep 3
PLAY16=$(node_id reac-playback.bmx1)
[ -n "$PLAY16" ] || { echo "FAIL: no reac-playback.bmx1"; exit 1; }
NP16=$(pw-dump | python3 -c '
import json,sys
d = json.load(sys.stdin); want = int(sys.argv[1]); n = 0
for o in d:
    if o.get("type") == "PipeWire:Interface:Port":
        p = o["info"]["props"]
        if int(p.get("node.id", -1)) == want and p.get("port.direction") == "in":
            n += 1
print(n)' "$PLAY16")
[ "$NP16" = "8" ] || {
	echo "FAIL: the 16-input box's playback door has $NP16 channels; an S-1608 has EIGHT"
	echo "      outputs, and what we send feeds its outputs"; exit 1; }
CAPD=$(node_prop reac-capture.bmx1 node.description)
case "$CAPD" in
  *S-1608*) : ;;
  *) echo "FAIL: the capture door is described '$CAPD' — it should name the box the way the"
     echo "      master path's does, because that is the operator-facing label"; exit 1 ;;
esac
echo "MEASURED: 16-in box master — playback door $NP16 ch (its outputs), capture door '$CAPD'"
kill -TERM $FAKE16 2>/dev/null; wait $FAKE16 2>/dev/null
ip link set bmx1 down

# ---- 8. AN ANNOUNCING MASTER IS NOT HUNTED, AND ITS TRANSFER IS NOT INTERRUPTED. ----
# The two rules the rig taught, one per master kind. A master that sends `cfea` (the S-1608
# in master mode) is found without a flood — the box that joined one broadcast NOTHING, where
# the box that joined the SILENT S-0808 flooded 0.68 s first. And both granted joins landed
# after the master's scene transfer stopped, +0.411 s and +0.217 s; the ksy says a box joining
# mid-transfer must not cancel it, and the emulator refuses an announce that arrives inside
# one, so a daemon that announces on its own clock fails here as it failed on the rig.
ip link add bmx2 type veth peer name mbx2 || exit 90
ip link set mbx2 netns $NSPID || exit 90
$in_peer "$FAKE" mbx2 00:40:ab:c4:08:cd 8 2000 "$RT/box2.rep" announcing >"$RT/box2.log" 2>&1 &
FAKE2=$!
sleep 0.5
ip link set bmx2 up; peer ip link set mbx2 up
wait_for "\[bmx2\] SLAVE role on a BOX MASTER" 30 || {
	echo "FAIL: the announcing box master was not joined"; tail -10 "$LOG"; exit 1; }
FL2=""
for i in $(seq 100); do
	FL2=$(rep_f flood 3 "$RT/box2.rep"); INS=$(awk '$1=="announce"{print $7}' "$RT/box2.rep")
	AOK2=$(awk '$1=="announce"{print $3}' "$RT/box2.rep")
	[ "$AOK2" = "1" ] && break
	sleep 0.5
done
[ "$AOK2" = "1" ] || {
	echo "FAIL: the announcing master never accepted our declaration (in_scene=${INS:-?})"
	cat "$RT/box2.rep"; exit 1; }
[ "${INS:-0}" = "0" ] || {
	echo "FAIL: $INS of our announces arrived INSIDE the master's scene transfer — a box"
	echo "      joining mid-transfer must not cancel it, and this master does not answer one"
	exit 1; }
wait_for "\[bmx2\] .*the master announces itself — no flood needed" 10 || {
	echo "FAIL: the master's cfea was never noticed"; tail -8 "$LOG"; exit 1; }
FL2=$(rep_f flood 3 "$RT/box2.rep")
# ONLY THE BOX GEOMETRY HAS A FLOOD TO SUPPRESS. In the mixer shape every audio frame is a
# broadcast downstream by the operator's ruling, so "broadcast frames before the announce" is
# not a flood at all and counting them as one would assert against the ruling. What both
# shapes share — the cfea noticed, the announce accepted, none of it inside the transfer — is
# asserted above for either.
[ "$WANT_LEN" = "340" ] || FL2=0
[ "${FL2:-0}" -lt 500 ] || {
	echo "FAIL: we broadcast $FL2 flood frames at a master that announces itself; a flood is"
	echo "      how a SILENT master is found, and noise at one that is calling"
	grep -E "\[bmx2\]" "$LOG" | tail -8; exit 1; }
echo "MEASURED: announcing master — $FL2 flood frames, $INS announces inside its transfer,"
echo "          declaration accepted"
kill -TERM $FAKE2 2>/dev/null; wait $FAKE2 2>/dev/null
ip link set bmx2 down

kill -TERM $FAKEPID 2>/dev/null; wait $FAKEPID 2>/dev/null
kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null
echo "PASS: a box master is ENROLLED WITH, its way — flood, announce, burst, grant, and its outputs carry our audio"
INNER
)
rc=$?
echo "$OUT"
[ $rc -eq 77 ] && exit 77
[ $rc -ne 0 ] && exit $rc
echo "$OUT" | grep -q "^PASS:" || { echo "FAIL: the inner namespace produced no verdict"; exit 1; }
exit 0
