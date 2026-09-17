#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: a segment pinned `role = box` PRESENTS ITS DECLARED ROW TO THE WIRE —
# the geometry, the firmware, the REAC version and the name — and the graph and the roster
# say the same thing the frames do.
# (docs/design/specs/2026-09-17-the-daemon-can-be-a-box.md §1, §2, §3, §5.)
#
# MEASURED AT THE FAR END OF THE CABLE, not in a log. The unit suite proves the synthesiser
# reproduces three real boxes byte for byte (libreac tests/test_box_table.c); what it cannot
# see is whether any of it reaches a wire — five features of this project were once built
# against empty function bodies with green tests over them. So the peer end of a veth runs a
# raw AF_PACKET sniffer that DECODES the declaration the way a mixer must: the twelve-slot
# port table, the identity page's firmware and REAC-version records, and the ASCII name
# split across two link-4 fragments. Every number asserted below is the ROW's, typed out.
#
# TWO ARMS, and the second is the operator's experiment (2026-09-17):
#   A  model = s1608  — a CAPTURED row: 16 in / 8 out, head-amp strap 2, firmware 2.200,
#      REAC 2.302, and NO name record, because the 0x82 family is named by its selector.
#      A real S-1608's own numbers, so this arm is the positive control for arm B.
#   B  model = fr4000 — a DERIVED row nobody has ever captured: the protocol's full
#      40-channel input width, with OUR identity on it — the name FR-4000, firmware 1.014
#      (this daemon's version) and REAC 9.014, a major no Roland box has ever sent.
#
# WHAT THIS DOES NOT PROVE, said here rather than discovered later: nothing GRANTS us. The
# flood is what a box does before any master answers, and reaching ESTABLISHED under a real
# mixer is the rig step in the spec's §9.
#
# ISOLATION: a user+net+mount+pid namespace with its own veth, its own sysfs and its own
# PipeWire on a private runtime dir — `unshare -n` isolates the wire and not the graph.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw}"
# ABSOLUTE, ALWAYS. nsenter into a mount namespace starts at /, so a relative binary path
# runs the box side and silently fails to start the master side — which read as "the mixer
# never enrolled us" until the log was looked at.
BIN=$(readlink -f "$BIN")
SKIP=77

for t in unshare nsenter ip pipewire pw-cli pw-dump python3; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
unshare -r -n -m -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+mount+pid namespaces unavailable"; exit $SKIP; }

OUT=$(unshare -r -n -m -p -f --mount-proc --map-root-user bash -s -- "$BIN" <<'INNER'
set -u
mount -t sysfs sysfs /sys 2>/dev/null || { echo "SKIP: cannot mount a private sysfs"; exit 77; }
BIN="$1"
LOG=$(mktemp); CONF=$(mktemp -d); RT=$(mktemp -d)
export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
cleanup() { kill -TERM $(jobs -p) 2>/dev/null; sleep 0.3; kill -9 $(jobs -p) 2>/dev/null;
            rm -rf "$LOG" "$CONF" "$RT"; }
trap cleanup EXIT

pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 50); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || {
	echo "SKIP: no private PipeWire in this namespace"; tail -3 "$RT/pw.log"; exit 77; }

# THE SNIFFER A MIXER WOULD BE. It reads REAC frames (EtherType 0x8819) off the peer end and
# decodes the three things a desk reads to know what box it is talking to. It prints one
# `key value` line per fact and NOTHING for a fact it never saw — an absent key must not
# read like a zero.
mkdir -p "$RT/mhome"
cat > "$RT/sniff.py" <<'PYEOF'
import socket, sys, time
iface, secs = sys.argv[1], float(sys.argv[2])
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x8819))
s.bind((iface, 0))
s.settimeout(0.5)
end = time.time() + secs
bcast = 0; announce = None; fw = None; ver = None; name_first = None; name_last = None
lens = {}
while time.time() < end:
    try:
        f = s.recv(2048)
    except socket.timeout:
        continue
    except OSError:
        # THE LINK BOUNCES UNDER US. reac-pw's wake ladder takes the netdev down and up
        # again when a box goes quiet, and a raw socket answers ENETDOWN for as long as
        # that lasts. Keep listening: a sniffer that dies on the first bounce measures
        # the seconds before it and calls them the whole run.
        time.sleep(0.2)
        continue
    if len(f) < 50:
        continue
    lens[len(f)] = lens.get(len(f), 0) + 1
    if f[0:6] == b'\xff\xff\xff\xff\xff\xff':
        bcast += 1
    blk = f[18:50]
    # cdea link 1, the config-announce: 01 03 00 10 then the selector
    if f[16:18] == b'\xcd\xea' and blk[0:4] == b'\x01\x03\x00\x10':
        announce = blk
    # the identity page rides link 4 with the DT1 address 0500 xxxx at blk[16:20]
    if f[16:18] == b'\xcd\xea' and blk[0] == 0x04 and blk[16:18] == b'\x05\x00':
        addr = blk[18:20]
        if blk[1] == 0x03 and addr == b'\x00\x00':
            fw = blk[20:24]
        elif blk[1] == 0x03 and addr == b'\x06\x00':
            ver = blk[20:28]
        elif blk[1] == 0x01 and addr == b'\x10\x00':
            name_first = blk
    if f[16:18] == b'\xcd\xea' and blk[0] == 0x04 and blk[1] == 0x02:
        name_last = blk
print("frames", sum(lens.values()))
print("broadcast", bcast)
for L in sorted(lens, key=lambda k: -lens[k])[:2]:
    print("len", L, lens[L])
if announce is not None:
    ins = sum(1 for b in announce[8:20] if b == 0x02) * 4
    outs = sum(1 for b in announce[8:20] if b == 0x01) * 4
    print("selector", "0x%02x" % announce[4])
    print("strap", announce[7])
    print("in", ins)
    print("out", outs)
    print("blocksum", sum(announce) % 256)
if fw is not None:
    print("fw", "%d.%d%d%d" % (fw[0], fw[1], fw[2], fw[3]))
if ver is not None:
    maj = ver[2] << 8 | ver[3]; mnr = ver[4] << 8 | ver[5]; pat = ver[6] << 8 | ver[7]
    print("reacver", "%d.%d%02d" % (maj, mnr, pat))
if name_first is not None:
    nm = bytes(name_first[21:31])
    if name_last is not None:
        nm += bytes(name_last[9:15])
    print("name", nm.split(b'\x00')[0].decode('ascii', 'replace'))
PYEOF

# ONE PROPERTY off one of this daemon's nodes, by node name; empty when it is not there.
node_prop() {
	pw-dump | python3 -c '
import json,sys
for o in json.load(sys.stdin):
    if o.get("type")=="PipeWire:Interface:Node" and \
       o["info"]["props"].get("node.name")==sys.argv[1]:
        print(o["info"]["props"].get(sys.argv[2],"")); break
' "$1" "$2"
}
# One key off the daemon's ROSTER node, whatever its index for this segment is.
roster_key() {   # roster_key <segment> <suffix>
	pw-dump | python3 -c '
import json,sys
seg,suf=sys.argv[1],sys.argv[2]
for o in json.load(sys.stdin):
    if o.get("type")!="PipeWire:Interface:Node": continue
    p=o["info"]["props"]
    # THE PROPS ARE JSON AND A NUMBER STAYS A NUMBER: pw-dump gives reac.roster as 1, not
    # "1", so a string compare finds the roster node never — an empty answer that reads
    # exactly like a daemon that publishes no roster at all.
    if str(p.get("reac.roster",""))!="1": continue
    n=int(p.get("reac.roster.n","0"))
    for i in range(n):
        if p.get("reac.roster.%d.name"%i)==seg:
            print(p.get("reac.roster.%d.%s"%(i,suf),"")); break
' "$1" "$2"
}

# ONE NODE NAME PER NODE THIS GRAPH CARRIES, so an empty lookup can be told from a wrong one.
node_names() { pw-dump | python3 -c '
import json,sys
for o in json.load(sys.stdin):
    if o.get("type")=="PipeWire:Interface:Node":
        print(o["info"]["props"].get("node.name","?"))
'; }

arm() {   # arm <tag> <model-token> <iface> [REAC_BOX_CHANNELS to be ignored]
	local tag="$1" model="$2" ifc="$3" peer="${3}p" envw="${4:-}"
	ip link add "$ifc" type veth peer name "$peer" || return 90
	unshare -n -m bash -c 'mount -t sysfs sysfs /sys 2>/dev/null; exec sleep 300' &
	local nspid=$!
	for i in $(seq 20); do nsenter -t $nspid -n -m true 2>/dev/null && break; sleep 0.1; done
	nsenter -t $nspid -n -m true 2>/dev/null || { echo "SKIP: no peer namespace"; return 77; }
	ip link set "$peer" netns $nspid || return 90
	ip link set "$ifc" up; nsenter -t $nspid -n -m ip link set "$peer" up

	mkdir -p "$CONF/.config/reac-pw"
	printf '[segment %s]\nrole = box\nmodel = %s\n' "$ifc" "$model" \
		> "$CONF/.config/reac-pw/reac-pw.conf"

	# THE MIXER IS THIS DAEMON'S OWN MASTER SIDE, in the peer namespace and on the same
	# private graph (a network namespace is not a mount namespace: it reaches the same
	# PipeWire socket). It is not a Roland desk and this test never claims it is — what
	# it provides is a REAC master that drives the establishment, which is what the box
	# side needs before it may announce itself. A box alone floods and says nothing else.
	nsenter -t $nspid -n -m env HOME="$RT/mhome" REAC_DEBUG=1 \
		"$BIN" --live "$peer" --tx "$peer" --name "$peer" --role master \
		>"$RT/$tag.master.log" 2>&1 &
	local mpid=$!
	nsenter -t $nspid -n -m python3 "$RT/sniff.py" "$peer" 22 > "$RT/$tag.sniff" 2>"$RT/$tag.snifferr" &
	local snpid=$!
	sleep 0.5
	# THE ENV CANNOT MOVE A BOX ROW'S WIDTH (spec §2a, §5). Arm A is launched with
	# REAC_BOX_CHANNELS set to a DIFFERENT legal width from its row's, so every width
	# asserted below is asserted against a key that is trying to change it; the key
	# must be named as ignored and nothing it names may move.
	HOME="$CONF" REAC_DEBUG=1 ${envw:+REAC_BOX_CHANNELS=$envw} "$BIN" >"$RT/$tag.log" 2>&1 &
	local pid=$!
	# SAMPLED OVER THE RUN, NOT ONCE AT THE END. The master's wake ladder takes the link
	# down and up when a box goes quiet, and every node and row on both sides is torn
	# down and rebuilt around that edge — a single sample lands in a gap and reports an
	# absence that never happened. What is claimed is that these facts WERE true, which
	# is what "the mixer enrolled us" means.
	local cap=0 play=0 rrole="" rwidth="" rmodel="" mmodel="none" mstate=""
	local i
	for ((i = 0; i < 40; i++)); do
		sleep 0.6
		[ "$(node_names | grep -c "^reac-capture.$ifc$")" -ge 1 ] && cap=1
		[ "$(node_names | grep -c "^reac-playback.$ifc$")" -ge 1 ] && play=1
		local v
		v=$(roster_key "$ifc" role);  [ -n "$v" ] && rrole="$v"
		# `0/0` IS WHAT A SEGMENT READS WHILE ITS PAIR IS BEING REBUILT, and it is a
		# legitimate value of this key — so it is skipped here the same way `none` is
		# for the model: what is claimed is that the declared width WAS published.
		v=$(roster_key "$ifc" width); [ -n "$v" ] && [ "$v" != "0/0" ] && rwidth="$v"
		v=$(roster_key "$ifc" model); [ -n "$v" ] && [ "$v" != "none" ] && rmodel="$v"
		v=$(roster_key "$peer" model); [ -n "$v" ] && [ "$v" != "none" ] && mmodel="$v"
		v=$(roster_key "$peer" state); [ "$v" = "established" ] && mstate="established"
	done
	kill -0 $pid 2>/dev/null || { echo "$tag daemon-died 1"; tail -5 "$RT/$tag.log" | sed "s/^/  $tag log /"; }
	kill -0 $mpid 2>/dev/null || { echo "$tag master-died 1"; tail -5 "$RT/$tag.master.log" | sed "s/^/  $tag mlog /"; }

	echo "$tag has-capture $cap"
	echo "$tag has-playback $play"
	echo "$tag roster-role $rrole"
	echo "$tag roster-width $rwidth"
	echo "$tag roster-model $rmodel"
	# WHAT THE MIXER MADE OF US: the master's own segment row, which is where a desk's
	# idea of the box it is driving lives. And how many JOINs it counted, which is the
	# fact the 40-channel experiment turns on.
	echo "$tag master-sees-model $mmodel"
	echo "$tag master-sees-state ${mstate:-not-established}"
	echo "$tag master-joins $(grep -ao "rx_joins=[0-9]*" "$RT/$tag.master.log" | tail -1 | cut -d= -f2)"
	echo "$tag env-width-ignored $(grep -ac "REAC_BOX_CHANNELS.*IGNORED" "$RT/$tag.log")"
	grep -a "BOX role" "$RT/$tag.log" | head -1 | sed "s/^/  $tag saidbox /"
	grep -aiE "establish|grant|enrol|announce" "$RT/$tag.log" | tail -4 | sed "s/^/  $tag boxlog /"
	grep -aiE "establish|grant|recogniz|autodetect|box" "$RT/$tag.master.log" | tail -5 | sed "s/^/  $tag mixlog /"
	wait $snpid 2>/dev/null
	sed "s/^/$tag /" "$RT/$tag.sniff"
	tail -2 "$RT/$tag.snifferr" | sed "s/^/  $tag snifferr /"
	kill -TERM $pid $mpid 2>/dev/null; sleep 0.5; kill -9 $pid $mpid 2>/dev/null
	kill -9 $nspid 2>/dev/null
	ip link del "$ifc" 2>/dev/null
	return 0
}

arm A s1608 bxa0 8 || exit $?
arm B fr4000 bxb0 || exit $?
exit 0
INNER
)
rc=$?
[ $rc -eq 0 ] || { echo "SKIP: the namespace body could not run (rc=$rc)"
                   echo "$OUT" | sed 's/^/  /'; exit $SKIP; }
echo "$OUT" | sed 's/^/  /'

get() {   # get <arm> <key>
	echo "$OUT" | awk -v a="$1" -v k="$2" '$1 == a && $2 == k { print $3; f=1; exit } END { exit !f }'
}
fail=0
say() { echo "FAIL: $*"; fail=1; }

# ---- THE INSTRUMENT FIRST. An absence measured by a sniffer that never saw a presence is
# ---- not a measurement: arm A must have carried REAC frames at all before any claim below.
FR=$(get A frames) || FR=0
[ "${FR:-0}" -gt 100 ] || say "the sniffer saw ${FR:-0} REAC frames on arm A — it cannot prove anything about arm B either"

# ---- ARM A: a CAPTURED row's own numbers reach the wire -------------------------------
[ "$(get A in)" = "16" ]      || say "arm A declared $(get A in) inputs on the wire; the S-1608 row says 16"
[ "$(get A out)" = "8" ]      || say "arm A declared $(get A out) outputs; the S-1608 row says 8"
[ "$(get A selector)" = "0x82" ] || say "arm A announced selector $(get A selector); the S-1608 row says 0x82"
[ "$(get A strap)" = "2" ]    || say "arm A announced head-amp strap $(get A strap); the S-1608 row says 2 (base 0x20)"
[ "$(get A blocksum)" = "0" ] || say "arm A's declaration does not checksum (sum mod 256 = $(get A blocksum))"
[ "$(get A fw)" = "2.200" ]   || say "arm A's firmware record reads '$(get A fw)'; the S-1608 row says 2.200"
[ "$(get A reacver)" = "2.302" ] || say "arm A's REAC version reads '$(get A reacver)'; the S-1608 row says 2.302"
NM=$(get A name) && say "arm A sent a NAME record ('$NM'); the 0x82 family is named by its selector and sends none"
[ "$(get A broadcast)" -gt 10 ] 2>/dev/null || say "arm A never flooded broadcast — a box announces itself before any master answers"

# ---- ARM B: the 40-channel experiment, with OUR identity on it -------------------------
[ "$(get B in)" = "40" ]      || say "arm B declared $(get B in) inputs; the experiment row says 40"
[ "$(get B out)" = "0" ]      || say "arm B declared $(get B out) outputs; the experiment row says 0"
[ "$(get B blocksum)" = "0" ] || say "arm B's declaration does not checksum (sum mod 256 = $(get B blocksum))"
[ "$(get B fw)" = "1.014" ]   || say "arm B's firmware reads '$(get B fw)'; our invented firmware is 1.014"
[ "$(get B reacver)" = "9.014" ] || say "arm B's REAC version reads '$(get B reacver)'; ours is 9.014"
[ "$(get B name)" = "FR-4000" ]  || say "arm B's name record reads '$(get B name)'; our invented name is FR-4000"

# ---- AND THE GRAPH SAYS THE SAME THING THE FRAMES DO -----------------------------------
[ "$(get A has-capture)" = "1" ]  || say "arm A published no reac-capture.bxa0 — a box's source is what the mixer sends it"
[ "$(get A has-playback)" = "1" ] || say "arm A published no reac-playback.bxa0 — a box's sink is what we send the mixer"
[ "$(get B has-capture)" = "1" ]  || say "arm B published no reac-capture.bxb0"
[ "$(get B has-playback)" = "1" ] || say "arm B published no reac-playback.bxb0"

# ---- AND THE MIXER ON THE OTHER END ENROLLED US AS THE ROW WE DECLARED -----------------
# The MIXER'S OWN ROSTER ROW for the wire we are on: what a desk thinks it is driving.
# Read there and not off its nodes, because a master with nothing recognised publishes no
# node at all (the no-box-no-node ruling) — an absence that would read like a wrong model.
[ "$(get A master-sees-model)" = "s1608" ] || say "the mixer read our model as '$(get A master-sees-model)', not s1608"
[ "$(get A master-sees-state)" = "established" ] || say "the mixer never established with us (state '$(get A master-sees-state)')"
# ---- THE 40-CHANNEL EXPERIMENT GETS AS FAR AS IT GETS, AND THAT IS THE MEASUREMENT ----
# The operator asked whether we can emulate a 40-channel box. What is asserted here is what
# was measured: the row reaches the wire whole (above) and our own master COUNTS ITS JOINS
# and grants them. It does NOT sustain presence at this width — the master's own journal
# says `no sustained presence` — so that is stated in the spec's §9 as an open question
# with a rig step, and is deliberately NOT asserted as a pass here. A test that claimed an
# enrolment nobody has seen would be the defect this whole file exists to catch.
JOINS=$(get B master-joins); JOINS=${JOINS:-0}
[ "$JOINS" -ge 1 ] 2>/dev/null || say "the mixer counted $JOINS joins from the 40-channel row — its cold-connect never reached a master at all"
[ "$(get A roster-role)" = "box" ] || say "arm A's roster reads role '$(get A roster-role)', not box"
[ "$(get B roster-role)" = "box" ] || say "arm B's roster reads role '$(get B roster-role)', not box"
[ "$(get A roster-width)" = "16/8" ] || say "arm A's roster width is '$(get A roster-width)', not 16/8"
[ "$(get B roster-width)" = "40/0" ] || say "arm B's roster width is '$(get B roster-width)', not 40/0"
[ "$(get A roster-model)" = "s1608" ] || say "arm A's roster model is '$(get A roster-model)'"
# ---- AND THE ENV DID NOT GET A VOTE ----------------------------------------------------
# Arm A ran with REAC_BOX_CHANNELS=8 against a 16/8 row. Every width above was measured
# under that key, so they are the claim's first half; this is the second, and it is the
# one that fails if the key is merely read and silently obeyed somewhere else later: a
# retired key must SAY it is ignored, or an operator cannot tell it from one that works.
[ "$(get A env-width-ignored)" -ge 1 ] 2>/dev/null ||
	say "arm A never named REAC_BOX_CHANNELS as ignored — the key is read under role = box and says nothing about it"
[ "$(get B roster-model)" = "fr4000" ] || say "arm B's roster model is '$(get B roster-model)'"

[ $fail -eq 0 ] && echo "OK: a box-role segment declares its row on the wire — the S-1608's own 16/8, strap 2, firmware 2.200, REAC 2.302, and the 40-channel experiment with our FR-4000 / 1.014 / 9.014 identity — and the roster says the same"
exit $fail
