#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: THE TRUNK'S VLANS, DECLARED ONCE, ARE MINTED COLD AND KNOCKED ON (reac-pw#99).
#
# THE COLD START A TRUNK CANNOT CLOSE BY HEARING. A trunk parent knocks untagged until tagged
# REAC is heard; only then are its VLAN sub-interfaces minted and knocked on (README, "A
# silent wire"). A cold box says nothing until a master speaks, and a box on an access port
# never hears an untagged knock. With every box cold nothing tagged ever arrives, so no VLAN
# is found: four boxes powered on behind the fibre switch before the console, and not one
# enrols, with no error anywhere. The owner's ruling on reac-pw#99 (2026-11-15) closes it with
# the smallest fix: the trunk's VLAN ids declared ONCE, as `[trunk <parent>]` / `vlans = ...`
# in the override file (the console writes the same words through its drop-in), each minted
# at start and knocked on in full, whether or not anything tagged was heard. The 2026-09-23
# rule for `[segment <parent>.<vid>]` stands beside it: that section still mints nothing.
#
# THE FAR END IS A VLAN THAT NEVER SPEAKS FIRST: libreac's fake_box behind farA.11 in a
# nested namespace, started BEFORE the daemon, silent until a master puts a COMPLETE scene
# transfer on its wire — the rig's boxes exactly. The kernel tags every frame both ways, so
# the daemon's knock reaches it only through a sub-interface for VID 11 that nothing heard
# can have named.
#
#   0. THE INSTRUMENT. The netdev reader sees a netdev it just made, its alias, and its
#      absence, before any absence below is believed.
#   a. COLD. `[trunk trunkA]` declares VIDs 11 and 33. Nothing has been transmitted. Within
#      seconds trunkA.11 and trunkA.33 exist, are UP and carry the mint alias; trunkA.44, a
#      VID nobody declared, does not (the mint is the declaration's, not a sweep's). The
#      mint line precedes the first tagged frame the parent ever hears. trunkA.11 is
#      KNOCKED on, the box ANSWERS it, the master takes the wire and ESTABLISHES; the box's
#      own log shows it spoke only after a complete push. trunkA.33 is knocked on, never
#      answered, and still there when 11 is established: silence does not release a
#      declared VLAN. A host-made trunkA.22 is served and never marked.
#   a2. NO KNOCK STORM. The knocks a declared VLAN nobody answers has opened, read from the
#      roster's own status line, stay within KNOCK.md's back-off over the window measured
#      (2.695 s doubling to 30 s: at most 1 + the periods that fit).
#   a3. LIVE (review of reac-pw#100: the console never restarts the daemon). The console's
#      drop-in rewrites the list mid-run: 55 added is minted, 33 withdrawn is removed (ours),
#      22 withdrawn is left alone (the host made it); 11 keeps its segment throughout.
#   b. LATE PARENT. The daemon starts before trunkB exists (the NetworkManager race on a
#      desk): it says so once, mints nothing, and mints trunkB.11 when trunkB appears.
#   c. EXIT. What it minted is gone; what the host made is untouched.
#
# SABOTAGE. REACPW_TEST_SABOTAGE=no-list writes the trunk section with NO `vlans` line: the
# declaration is refused by name, nothing is minted, and this test goes RED at arm a, which
# is the proof that arm a measures the declaration and not the knock alone.
#
# ISOLATION: a user+net+mount+pid namespace with its own veth, its own sysfs (fake_box reads
# its carrier there) and its own PipeWire on a private runtime dir; the far end lives in a
# nested network+mount namespace so its VLAN sub-interface is not a netdev the daemon under
# test could see and serve. HOME is redirected at the daemon so the conf this test writes is
# the only one it reads.
set -u
. "$(dirname "$0")/facts.sh"   # FACT_<NAME>: the protocol's numbers, from their one declaration
BIN="${1:?usage: $0 /path/to/reac-pw /path/to/fake_box}"
FAKE="${2:-}"
SKIP=77
SABOTAGE="${REACPW_TEST_SABOTAGE:-}"

[ -n "$FAKE" ] && [ -x "$FAKE" ] || { echo "SKIP: no fake_box at '$FAKE' (libreac: make fake_box)"; exit $SKIP; }
for t in unshare nsenter ip pipewire pw-cli pw-dump python3; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
unshare -r -n -m -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+mount+pid namespaces unavailable"; exit $SKIP; }
# THE KERNEL'S LINK TYPES ARE PROBED BY NAME (audit 2026-09-24, H3): a kernel without 8021q
# is a machine this test cannot run on, and says so here, so a later `|| exit 90` is a FAIL.
unshare -r -n sh -c 'ip link add p0 type veth peer name p1 && ip link add link p0 name p0.9 type vlan id 9' 2>/dev/null || {
	echo "SKIP: this kernel cannot create a VLAN link in a namespace (no 8021q)"; exit $SKIP; }

OUT=$(REACPW_TEST_LOGDIR="${REACPW_TEST_LOGDIR:-}" unshare -r -n -m -p -f --mount-proc --map-root-user bash -s -- "$BIN" "$FAKE" "$SABOTAGE" <<'INNER'
set -u
mount -t sysfs sysfs /sys 2>/dev/null || { echo "SKIP: cannot mount a private sysfs"; exit 77; }
BIN="$1"; FAKE="$2"; SABOTAGE="$3"
RT=$(mktemp -d)
cleanup() {
	kill -TERM $(jobs -p) 2>/dev/null
	sleep 0.3
	kill -9 $(jobs -p) 2>/dev/null
	# the whole transcript of both ends, for a bench that wants to read what the box saw
	[ -n "${REACPW_TEST_LOGDIR:-}" ] && [ -d "$REACPW_TEST_LOGDIR" ] && cp "$RT"/*.log "$REACPW_TEST_LOGDIR"/ 2>/dev/null
	rm -rf "$RT"
}
trap cleanup EXIT

export XDG_RUNTIME_DIR="$RT" PIPEWIRE_RUNTIME_DIR="$RT"
pipewire >"$RT/pw.log" 2>&1 &
for i in $(seq 50); do pw-cli info 0 >/dev/null 2>&1 && break; sleep 0.2; done
pw-cli info 0 >/dev/null 2>&1 || { echo "SKIP: no private PipeWire in the namespace"; exit 77; }
echo "private-graph-socket $RT"

CONF="$RT/home"
mkdir -p "$CONF/.config/reac-pw"

# What is on this host, as the kernel answers it over netlink: `present`/`absent`, the
# operational flag, and the interface ALIAS, which is the mint mark reac_vlan writes. Read
# from `ip`, not /sys: the alias is a netlink attribute and the reader must not depend on
# which sysfs is mounted.
netdev() {
	if ! ip -o link show "$1" >/dev/null 2>&1; then echo "absent"; return; fi
	local up al
	up=$(ip -o link show "$1" | grep -qw UP && echo up || echo down)
	al=$(ip -d -o link show "$1" 2>/dev/null | tr '\\' '\n' \
	     | sed -n 's/^ *alias \(.*\)$/\1/p' | head -1)
	echo "present $up alias=${al:-none}"
}
wait_for() {   # wait_for <regex> <secs> <file>
	for ((i = 0; i < $2 * 2; i++)); do grep -aq "$1" "$3" && return 0; sleep 0.5; done
	return 1
}
wait_present() {   # wait_present <netdev> <secs>
	for ((i = 0; i < $2 * 2; i++)); do
		[ "$(netdev "$1" | awk '{print $1}')" = "present" ] && return 0
		sleep 0.5
	done
	return 1
}
wait_absent() {   # wait_absent <netdev> <secs>
	for ((i = 0; i < $2 * 2; i++)); do
		[ "$(netdev "$1" | awk '{print $1}')" = "absent" ] && return 0
		sleep 0.5
	done
	return 1
}
# The roster's status of one segment, from the daemon's own node on the private graph (the
# idiom of roster-node-lists-every-segment.sh): the knocks it has opened are in that sentence.
roster_status() {   # roster_status <daemon pid> <segment name>
	pw-dump 2>/dev/null | python3 -c '
import json,sys
pid=int(sys.argv[1]); name=sys.argv[2]
d=json.load(sys.stdin)
mine={o["id"] for o in d if o.get("type")=="PipeWire:Interface:Client"
      and int(o["info"]["props"].get("application.process.id",-1))==pid}
for o in d:
    if o.get("type")!="PipeWire:Interface:Node": continue
    p=o["info"]["props"]
    if int(p.get("client.id",-1)) not in mine or str(p.get("reac.roster"))!="1": continue
    for k,v in p.items():
        if k.startswith("reac.roster.") and k.endswith(".name") and v==name:
            print(p.get(k[:-len("name")]+"status","")); break
' "$1" "$2"
}

# ---- 0. THE INSTRUMENT, in every direction it is about to be trusted in.
ip link add ctrl0 type dummy 2>/dev/null || ip link add ctrl0 type veth peer name ctrl1
echo "control-unmarked $(netdev ctrl0)"
ip link set dev ctrl0 alias reac-pw:minted
echo "control-present $(netdev ctrl0)"
ip link del ctrl0 2>/dev/null
echo "control-absent $(netdev ctrl0)"

# The far end is another host in its own namespaces, with a sysfs of its own for fake_box.
unshare -n -m bash -c 'mount -t sysfs sysfs /sys 2>/dev/null; exec sleep 900' &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n -m true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n -m true 2>/dev/null || {
	echo "SKIP: no nested network+mount namespace for the far end"; exit 77; }
peer() { nsenter -t $NSPID -n -m "$@"; }

ip link add trunkA type veth peer name farA || exit 90
ip link set farA netns $NSPID || exit 90
ip link set trunkA up; peer ip link set farA up
# The host-made control: the daemon serves it if anything is there and never owns it.
ip link add link trunkA name trunkA.22 type vlan id 22 || exit 90
ip link set trunkA.22 up
# THE BOX, FIRST AND SILENT, behind VID 11 on the far end: the kernel tags everything it
# will ever say, and it says nothing until a master pushes a whole scene at it.
# NO HOST CHATTER ON THE BOX'S VLAN. A tag on ANY ethertype names a VLAN (the 2026-09-22
# rule), and a Linux sub-interface coming up speaks IPv6 by itself (MLD reports, a router
# solicitation): on the first run of this test those two frames named VID 11 within a
# second and the daemon minted it by HEARING. A box's access port carries the box and nothing
# else, so the far end's VLAN is made mute first, and the order check below (the mint before
# the first tagged frame) is what keeps this honest.
peer ip link add link farA name farA.11 type vlan id 11 || exit 90
peer ip link set farA.11 addrgenmode none
peer sh -c 'echo 1 > /proc/sys/net/ipv6/conf/farA.11/disable_ipv6' || exit 90
peer ip link set farA.11 up
echo "a-far-ipv6-off $(peer cat /proc/sys/net/ipv6/conf/farA.11/disable_ipv6)"
peer "$FAKE" farA.11 240 >"$RT/box.log" 2>&1 &
BOXPID=$!
sleep 1

{
	echo "[trunk trunkA]"
	[ "$SABOTAGE" = "no-list" ] || echo "vlans = 11 33"
	echo
	echo "# the 2026-09-23 rule stands beside the trunk's list: this names a VLAN if it exists"
	echo "[segment trunkA.44]"
} > "$CONF/.config/reac-pw/reac-pw.conf"

echo "a-before-11 $(netdev trunkA.11)"
echo "a-before-22 $(netdev trunkA.22)"
echo "a-before-33 $(netdev trunkA.33)"
echo "a-before-44 $(netdev trunkA.44)"
echo "a-box-spoke-before $(grep -ac 'sent' "$RT/box.log")"

T0=$(date +%s)
HOME="$CONF" REAC_DEBUG=1 "$BIN" >"$RT/a.log" 2>&1 &
APID=$!
sleep 1
kill -0 $APID 2>/dev/null || { echo "a-daemon-died"; tail -5 "$RT/a.log"; exit 1; }

# ---- a. COLD: the declaration mints, with nothing heard.
wait_present trunkA.11 10 && wait_present trunkA.33 10
sleep 1
echo "a-minted-11 $(netdev trunkA.11)"
echo "a-minted-22 $(netdev trunkA.22)"
echo "a-minted-33 $(netdev trunkA.33)"
echo "a-minted-44 $(netdev trunkA.44)"
grep -aE "declared trunk|declared on trunk|declares no vlans|\[trunk trunkA\]" "$RT/a.log" | tr -d '\r' | sed 's/^/  a: /' | head -6

# The knock on the declared VLAN, and the box's answer to it.
wait_for "\[trunkA.11\] nothing heard in .* KNOCKING" 20 "$RT/a.log" && echo "a-knocked-11 1" || echo "a-knocked-11 0"
wait_for "\[trunkA.11\] ANSWERED" 40 "$RT/a.log" && echo "a-answered-11 1" || echo "a-answered-11 0"
wait_for "\[trunkA.11\] the wire carried nothing for .* before the knock and a box answered it; taking the wire as MASTER now" 5 "$RT/a.log" \
	&& echo "a-master-11 1" || echo "a-master-11 0"
wait_for "GRANTING -> ESTABLISHED" 15 "$RT/a.log" && echo "a-established-11 1" || echo "a-established-11 0"
wait_for "\[trunkA.33\] nothing heard in .* KNOCKING" 20 "$RT/a.log" && echo "a-knocked-33 1" || echo "a-knocked-33 0"
echo "a-answered-33 $(grep -ac '\[trunkA.33\] ANSWERED' "$RT/a.log")"
# THE ORDER IS THE PROOF OF COLD: the mint line comes before the first tagged frame the
# parent ever heard, or the VLAN was found by hearing after all.
mint_at=$(grep -an '\[trunkA.11\] declared on trunk trunkA' "$RT/a.log" | head -1 | cut -d: -f1)
heard_at=$(grep -an '\[trunkA\] tagged VLAN heard' "$RT/a.log" | head -1 | cut -d: -f1)
echo "a-order mint=${mint_at:-none} heard=${heard_at:-none}"
echo "a-box-answered $(grep -ac 'commit report sent after a COMPLETE' "$RT/box.log")"
echo "a-still-33 $(netdev trunkA.33)"
echo "a-still-44 $(netdev trunkA.44)"
grep -aE "\[trunkA(\.11|\.33)?\] (nothing heard|ANSWERED|taking the wire|tagged VLAN heard)|GRANTING ->" "$RT/a.log" | tr -d '\r' | sed 's/^/  a: /' | head -8

# ---- a2. NO KNOCK STORM: the knocks trunkA.33 has opened, against the back-off over the
# window since the daemon started (the window overstates: the knock opened later than that).
# The box answers within two seconds, so the window is held open to twelve: by then the
# back-off has opened the third knock (0, 2.695, 8.084 s) and not the fourth (18.862 s).
while [ $(( $(date +%s) - T0 )) -lt 12 ]; do sleep 1; done
W=$(( $(date +%s) - T0 ))
echo "a-window-s $W"
echo "a-roster-33 $(roster_status $APID trunkA.33)"

# ---- a3. LIVE: the console's drop-in takes the list over, mid-run.
mkdir -p "$CONF/.config/reac-pw/reac-pw.conf.d"
cat > "$CONF/.config/reac-pw/reac-pw.conf.d/50-openmixer.conf" <<EOF
[trunk trunkA]
vlans = 11 22 55
EOF
wait_present trunkA.55 6 && wait_absent trunkA.33 6
sleep 0.5
echo "live1-55 $(netdev trunkA.55)"
echo "live1-33 $(netdev trunkA.33)"
echo "live1-22 $(netdev trunkA.22)"
echo "live1-11 $(netdev trunkA.11)"
cat > "$CONF/.config/reac-pw/reac-pw.conf.d/50-openmixer.conf" <<EOF
[trunk trunkA]
vlans = 11
EOF
wait_absent trunkA.55 6
sleep 0.5
echo "live2-55 $(netdev trunkA.55)"
echo "live2-22 $(netdev trunkA.22)"
echo "live2-11 $(netdev trunkA.11)"
echo "live2-established $(grep -ac 'GRANTING -> ESTABLISHED' "$RT/a.log")"
grep -aE "declared trunk \(reac-pw.conf.d|added to trunk|withdrawn from" "$RT/a.log" | tr -d '\r' | sed 's/^/  a: /' | head -8

# ---- c. EXIT, arm a.
kill -TERM $APID 2>/dev/null
for i in $(seq 40); do kill -0 $APID 2>/dev/null || break; sleep 0.2; done
kill -9 $APID 2>/dev/null; wait $APID 2>/dev/null
echo "a-after-11 $(netdev trunkA.11)"
echo "a-after-22 $(netdev trunkA.22)"
echo "a-after-33 $(netdev trunkA.33)"
grep -a "removed — declared on its trunk" "$RT/a.log" | tr -d '\r' | sed 's/^/  a: /' | head -2
kill -TERM $BOXPID 2>/dev/null

# ---- b. LATE PARENT: the daemon starts first, the trunk appears three seconds later.
cat > "$CONF/.config/reac-pw/reac-pw.conf" <<EOF
[trunk trunkB]
vlans = 11
EOF
echo "b-before-parent $(netdev trunkB)"
HOME="$CONF" REAC_DEBUG=1 "$BIN" >"$RT/b.log" 2>&1 &
BPID=$!
sleep 3
kill -0 $BPID 2>/dev/null || { echo "b-daemon-died"; tail -5 "$RT/b.log"; exit 1; }
echo "b-noparent-11 $(netdev trunkB.11)"
wait_for "declared trunk trunkB is not on this host yet" 2 "$RT/b.log" && echo "b-said-waiting 1" || echo "b-said-waiting 0"
ip link add trunkB type veth peer name farB || exit 90
ip link set farB netns $NSPID || exit 90
ip link set trunkB up; peer ip link set farB up
wait_present trunkB.11 10
sleep 0.5
echo "b-parent-11 $(netdev trunkB.11)"
kill -TERM $BPID 2>/dev/null
for i in $(seq 40); do kill -0 $BPID 2>/dev/null || break; sleep 0.2; done
kill -9 $BPID 2>/dev/null; wait $BPID 2>/dev/null
echo "b-after-11 $(netdev trunkB.11)"
exit 0
INNER
)
rc=$?
# THE BODY'S rc IS A VERDICT (audit 2026-09-24, H3): a dead daemon FAILs whatever rc it left,
# 77 is the only SKIP, any other rc FAILs.
echo "$OUT" | grep -qa 'daemon-died' && { echo "$OUT" | sed 's/^/  /'; echo "FAIL: the daemon died at start"; exit 1; }
[ $rc -eq 77 ] && { echo "$OUT" | sed 's/^/  /'; exit $SKIP; }
[ $rc -eq 0 ] || { echo "$OUT" | sed 's/^/  /'; echo "FAIL: the namespace body exited rc=$rc"; exit 1; }

echo "$OUT" | sed 's/^/  /'

fail() { echo "FAIL: $1"; exit 1; }
line() { echo "$OUT" | grep -a "^$1 " | head -1; }
st()   { line "$1" | awk '{print $2}'; }
upness() { line "$1" | awk '{print $3}'; }
alias_of() { line "$1" | sed -n 's/.*alias=\(.*\)$/\1/p'; }

# 0. THE INSTRUMENT WORKS IN EVERY DIRECTION IT IS ABOUT TO BE TRUSTED IN.
[ "$(st control-present)" = "present" ] \
	|| fail "the reader could not SEE a netdev that had just been created — every absence below would be a broken search"
[ "$(st control-absent)" = "absent" ] \
	|| fail "the reader reported a netdev after it was deleted"
[ "$(alias_of control-present)" = "reac-pw:minted" ] \
	|| fail "the reader could not see an alias that had just been SET: $(line control-present)"
[ "$(alias_of control-unmarked)" = "none" ] \
	|| fail "the reader invented an alias on a netdev that carries none: $(line control-unmarked)"
[ "$(st a-before-11)" = "absent" ] || fail "trunkA.11 existed before the daemon ran: $(line a-before-11)"
[ "$(st a-before-33)" = "absent" ] || fail "trunkA.33 existed before the daemon ran: $(line a-before-33)"
[ "$(st a-before-22)" = "present" ] || fail "the host-made control was not set up"
[ "$(st a-box-spoke-before)" = "0" ] || fail "the box spoke before the daemon ran — this is not a cold start"
[ "$(st a-far-ipv6-off)" = "1" ] || fail "the far end's VLAN still speaks IPv6, so its own chatter would name VID 11: not a cold start"

# a. COLD: THE DECLARATION MINTS, AND NOTHING HAD TO BE HEARD. Red before reac-pw#99.
[ "$(st a-minted-11)" = "present" ] \
	|| fail "trunkA.11 is declared on its trunk, nothing tagged was heard, and it was NOT minted — the cold start of reac-pw#99: $(line a-minted-11)"
[ "$(upness a-minted-11)" = "up" ] || fail "trunkA.11 was minted and left DOWN: $(line a-minted-11)"
[ "$(alias_of a-minted-11)" = "reac-pw:minted" ] \
	|| fail "trunkA.11 carries no mint alias, so the next start would inherit it as the host's: $(line a-minted-11)"
[ "$(st a-minted-33)" = "present" ] && [ "$(upness a-minted-33)" = "up" ] \
	|| fail "trunkA.33, declared with nobody behind it, was not minted and up: $(line a-minted-33)"
[ "$(alias_of a-minted-33)" = "reac-pw:minted" ] || fail "trunkA.33 carries no mint alias: $(line a-minted-33)"
[ "$(st a-minted-44)" = "absent" ] \
	|| fail "trunkA.44 was minted and nobody declared it on the trunk — a [segment] section still mints nothing (2026-09-23), and this is no sweep: $(line a-minted-44)"
[ "$(alias_of a-minted-22)" = "none" ] \
	|| fail "the daemon marked a netdev the HOST created as its own: $(line a-minted-22)"

# THE KNOCK, THE ANSWER, THE MASTER — through a VLAN nothing heard could have named.
[ "$(st a-knocked-11)" = "1" ] || fail "the declared trunkA.11 was never knocked on"
[ "$(st a-answered-11)" = "1" ] || fail "the box behind VID 11 never answered the knock (fake_box answers a COMPLETE push)"
[ "$(st a-master-11)" = "1" ] || fail "the box answered and the master did not take trunkA.11"
[ "$(st a-established-11)" = "1" ] || fail "the master took trunkA.11 and never established"
[ "$(st a-box-answered)" -ge 1 ] 2>/dev/null || fail "the box's own log shows no commit report after a complete push: the answer did not come from the box"
mint_at=$(line a-order | sed -n 's/.*mint=\([^ ]*\).*/\1/p'); heard_at=$(line a-order | sed -n 's/.*heard=\([^ ]*\).*/\1/p')
[ "$mint_at" != "none" ] || fail "the daemon never said it minted trunkA.11 on its declared trunk"
[ "$heard_at" != "none" ] || fail "no tagged frame was ever heard on trunkA — the box's answer did not land, so the order below is not a measurement"
[ "$mint_at" -lt "$heard_at" ] || fail "the mint of trunkA.11 came AFTER the first tagged frame on trunkA (line $mint_at vs $heard_at): it was found by hearing, not declared cold"

# SILENCE DOES NOT RELEASE A DECLARED VLAN: 33 was knocked on, nobody answered, it stays.
[ "$(st a-knocked-33)" = "1" ] || fail "the declared trunkA.33 was never knocked on"
[ "$(st a-answered-33)" = "0" ] || fail "trunkA.33 has nobody behind it and was ANSWERED: $(line a-answered-33)"
[ "$(st a-still-33)" = "present" ] || fail "trunkA.33 went away in silence while its sibling established — a declared VLAN outlives silence: $(line a-still-33)"
[ "$(st a-still-44)" = "absent" ] || fail "trunkA.44 appeared later with nothing declaring it: $(line a-still-44)"

# a2. NO KNOCK STORM. KNOCK.md: knock n+1 opens 2.695 s x 2^n after knock n, capped at 30 s,
# so in W seconds at most 1 + (the periods whose running sum fits in W) knocks open.
knocks=$(line a-roster-33 | sed -n 's/.*nothing heard: knock \([0-9]*\),.*/\1/p')
[ -n "$knocks" ] || fail "the roster's status of trunkA.33 does not say how many knocks it opened: $(line a-roster-33)"
allowed=$(awk -v w="$(st a-window-s)" 'BEGIN { n = 1; t = 0; p = 2.695; while (t + p <= w) { t += p; n++; p = (p * 2 > 30) ? 30 : p * 2 }; print n }')
[ "$knocks" -le "$allowed" ] \
	|| fail "trunkA.33 opened $knocks knocks in $(st a-window-s) s; the back-off allows at most $allowed — a knock storm"
[ "$knocks" -ge 2 ] \
	|| fail "trunkA.33 opened only $knocks knock(s) in $(st a-window-s) s: the back-off's second knock (2.695 s) never came, or the count is not counting"

# a3. LIVE: the list rewritten by the console's drop-in is followed without a restart.
[ "$(st live1-55)" = "present" ] && [ "$(alias_of live1-55)" = "reac-pw:minted" ] \
	|| fail "VID 55 added to the drop-in's list mid-run was not minted: $(line live1-55)"
[ "$(st live1-33)" = "absent" ] || fail "VID 33 withdrawn by the drop-in's list stayed minted: $(line live1-33)"
[ "$(st live1-22)" = "present" ] && [ "$(alias_of live1-22)" = "none" ] \
	|| fail "the host-made trunkA.22, listed by the drop-in, was marked or removed: $(line live1-22)"
[ "$(st live1-11)" = "present" ] || fail "VID 11 kept through the rewrite went away: $(line live1-11)"
[ "$(st live2-55)" = "absent" ] || fail "VID 55 withdrawn again stayed minted: $(line live2-55)"
[ "$(st live2-22)" = "present" ] && [ "$(alias_of live2-22)" = "none" ] \
	|| fail "the host-made trunkA.22 withdrawn from the list was removed or marked — it was never ours: $(line live2-22)"
[ "$(st live2-11)" = "present" ] || fail "VID 11 went away on the second rewrite: $(line live2-11)"
[ "$(st live2-established)" = "1" ] || fail "the rewrites re-established or dropped the box's segment ($(st live2-established) establishments)"
echo "$OUT" | grep -aq 'a: .*withdrawn from its trunk' || fail "the daemon never said a withdrawn VLAN was let go"
echo "$OUT" | grep -aq 'a: .*added to trunk trunkA' || fail "the daemon never said an added VLAN was minted"

# b. THE LATE PARENT.
[ "$(st b-before-parent)" = "absent" ] || fail "trunkB existed before its arm began"
[ "$(st b-noparent-11)" = "absent" ] || fail "trunkB.11 was minted with no trunkB on the host: $(line b-noparent-11)"
[ "$(st b-said-waiting)" = "1" ] || fail "the daemon never said the declared trunk trunkB was not on the host yet"
[ "$(st b-parent-11)" = "present" ] && [ "$(upness b-parent-11)" = "up" ] \
	|| fail "trunkB appeared and the declared trunkB.11 was not minted on it: $(line b-parent-11)"
[ "$(alias_of b-parent-11)" = "reac-pw:minted" ] || fail "trunkB.11 carries no mint alias: $(line b-parent-11)"

# c. THE EXIT. What it minted is gone; what the host made is untouched.
[ "$(st a-after-11)" = "absent" ] || fail "the daemon exited and left trunkA.11 behind: $(line a-after-11)"
[ "$(st a-after-33)" = "absent" ] || fail "trunkA.33 is back after the exit: $(line a-after-33)"
[ "$(st a-after-22)" = "present" ] \
	|| fail "the daemon removed a netdev the HOST created — it was never ours to take: $(line a-after-22)"
[ "$(st b-after-11)" = "absent" ] || fail "the daemon exited and left trunkB.11 behind: $(line b-after-11)"

echo "OK"
exit 0
