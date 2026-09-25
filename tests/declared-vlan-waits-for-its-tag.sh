#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: a DECLARED VLAN segment is NOT minted until its tag is heard — and is minted
# the moment it is. (docs/design/specs/2026-09-16-segments-and-roles-are-autodetected.md,
# amendment 2026-09-23: "we don't carry any VLANs if we don't detect VLANs".)
#
# WHAT THIS REPLACES. Until 2026-09-23 this file was declared-vlan-is-minted.sh and asserted
# the opposite: a declared `<parent>.<vid>` minted at start with nothing ever heard, the
# cold-boot fix of 2026-09-15. The desk paid for that rule every boot
# (docs/design/evidence/reac-pw-boot-2026-09-23.log lines 15-47, 101-131): three VLANs minted
# on a parent that hears no tag at all, three vacant tap doors, three roster rows, all of it
# re-created after every drop of the parent. The 2026-09-22 rule hears a VID from ANY tagged
# frame, so a cold trunk names its VLANs by itself; the declaration is what pins the role.
#
#   1. DECLARED, PARENT PRESENT, SILENCE. Two VIDs are declared in the daemon's own conf
#      file (one with a role, one bare) and nothing is transmitted. Four seconds later
#      NEITHER netdev exists. A host-made sub-interface on the same parent is untouched.
#   2. THE TAG ARRIVES. libreac's fake box master transmits inside VID 11 on the far end
#      — the kernel tags every frame — and `<parent>.11` appears, up, carrying the mint
#      alias, with the declared role reported on it. This is the positive control: a
#      detector that never minted anything could not tell this test from a broken one.
#   3. EXIT. What it minted is gone; what the host made is untouched.
#
# THE INSTRUMENT IS PROVEN BEFORE ANY ABSENCE IS BELIEVED: a netdev is created by hand and
# the same reader that will report absences is required to SEE it, and its alias, first.
#
# ISOLATION: a user+net+pid namespace with its own veth AND its own PipeWire on a private
# runtime dir; the far end of the trunk lives in a nested network namespace so its VLAN
# sub-interface is not a netdev the daemon under test could see and serve. HOME is
# redirected at the daemon so the conf file this test writes is the only one it reads.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw /path/to/fake-box-master}"
FAKE="${2:?usage: $0 /path/to/reac-pw /path/to/fake-box-master}"
SKIP=77

for t in unshare nsenter ip pipewire pw-cli; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
[ -x "$FAKE" ] || { echo "SKIP: no fake-box-master at '$FAKE'"; exit $SKIP; }
unshare -r -n -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+pid namespaces unavailable"; exit $SKIP; }
# THE KERNEL'S LINK TYPES ARE PROBED BY NAME (audit 2026-09-24, H3): a kernel without 8021q
# is a machine this test cannot run on, and says so here, so a later `|| exit 90` is a FAIL.
unshare -r -n sh -c 'ip link add p0 type veth peer name p1 && ip link add link p0 name p0.9 type vlan id 9' 2>/dev/null || {
	echo "SKIP: this kernel cannot create a VLAN link in a namespace (no 8021q)"; exit $SKIP; }

OUT=$(unshare -r -n -p -f --mount-proc --map-root-user bash -s -- "$BIN" "$FAKE" <<'INNER'
set -u
BIN="$1"; FAKE="$2"
RT=$(mktemp -d)
cleanup() {
	kill -TERM $(jobs -p) 2>/dev/null
	sleep 0.3
	kill -9 $(jobs -p) 2>/dev/null
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
# from `ip`, not /sys: this namespace inherits the host's sysfs (declared-vlan-is-minted.sh
# met a reader that could not see the alias it had just set for exactly that reason).
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

ip link add ctrl0 type dummy 2>/dev/null || ip link add ctrl0 type veth peer name ctrl1
echo "control-unmarked $(netdev ctrl0)"
ip link set dev ctrl0 alias reac-pw:minted
echo "control-present $(netdev ctrl0)"
ip link del ctrl0 2>/dev/null
echo "control-absent $(netdev ctrl0)"

# The far end is another host in its own namespace.
unshare -n sleep 600 &
NSPID=$!
for i in $(seq 20); do nsenter -t $NSPID -n true 2>/dev/null && break; sleep 0.1; done
nsenter -t $NSPID -n true 2>/dev/null || {
	echo "SKIP: no nested network namespace for the far end"; exit 77; }
peer() { nsenter -t $NSPID -n "$@"; }

ip link add trunkA type veth peer name farA || exit 90
ip link set farA netns $NSPID || exit 90
ip link set trunkA up; peer ip link set farA up
# The host-made control: the daemon serves it if anything is there and never owns it.
ip link add link trunkA name trunkA.22 type vlan id 22 || exit 90
ip link set trunkA.22 up

cat > "$CONF/.config/reac-pw/reac-pw.conf" <<EOF
[segment trunkA.11]
role = master

# A SECTION THAT SAYS NOTHING IS STILL A DECLARATION — and mints nothing either.
[segment trunkA.33]
EOF

echo "a-before-11 $(netdev trunkA.11)"
echo "a-before-22 $(netdev trunkA.22)"
echo "a-before-33 $(netdev trunkA.33)"

HOME="$CONF" REAC_DEBUG=1 "$BIN" >"$RT/a.log" 2>&1 &
APID=$!
sleep 4
kill -0 $APID 2>/dev/null || { echo "a-daemon-died"; tail -5 "$RT/a.log"; exit 1; }
# ---- 1. SILENCE. Nothing has been transmitted on this wire, so nothing is carried.
echo "a-silent-11 $(netdev trunkA.11)"
echo "a-silent-22 $(netdev trunkA.22)"
echo "a-silent-33 $(netdev trunkA.33)"
grep -a "declared VLAN segment" "$RT/a.log" | tr -d '\r' | sed 's/^/  a: /' | head -2

# ---- 2. THE TAG. A box master inside VID 11 on the far end; the kernel tags every frame.
peer ip link add link farA name farA.11 type vlan id 11 || exit 90
peer ip link set farA.11 up
peer "$FAKE" farA.11 00:40:ab:c4:11:21 8 2000 >"$RT/tag.log" 2>&1 &
TAGPID=$!
if wait_for "\[trunkA\] tagged .* vid 11" 30 "$RT/a.log"; then
	echo "a-heard-11 1"
else
	echo "a-heard-11 0"
fi
for ((i = 0; i < 40; i++)); do
	[ "$(netdev trunkA.11 | awk '{print $1}')" = "present" ] && break
	sleep 0.5
done
sleep 1
echo "a-tagged-11 $(netdev trunkA.11)"
echo "a-tagged-33 $(netdev trunkA.33)"
if wait_for "\[trunkA.11\] listening — role master (reac-pw.conf)" 15 "$RT/a.log"; then
	echo "a-role-11 master"
else
	echo "a-role-11 none"
fi
grep -aE "\[trunkA\] tagged|\[trunkA.11\] (listening|vid|declared)|vid 11" "$RT/a.log" | tr -d '\r' | sed 's/^/  a: /' | head -6

# ---- 3. EXIT.
kill -TERM $TAGPID 2>/dev/null
kill -TERM $APID 2>/dev/null
for i in $(seq 40); do kill -0 $APID 2>/dev/null || break; sleep 0.2; done
kill -9 $APID 2>/dev/null; wait $APID 2>/dev/null
echo "a-after-11 $(netdev trunkA.11)"
echo "a-after-22 $(netdev trunkA.22)"
echo "a-after-33 $(netdev trunkA.33)"
exit 0
INNER
)
rc=$?
# THE BODY'S rc IS A VERDICT (audit 2026-09-24, H3): a dead daemon FAILs whatever rc it left,
# 77 is the only SKIP, any other rc FAILs. Any-non-zero-is-SKIP read a crash at start as green.
echo "$OUT" | grep -qa 'daemon-died' && { echo "$OUT" | sed 's/^/  /'; echo "FAIL: the daemon died at start"; exit 1; }
[ $rc -eq 77 ] && { echo "$OUT" | sed 's/^/  /'; exit $SKIP; }
[ $rc -eq 0 ] || { echo "$OUT" | sed 's/^/  /'; echo "FAIL: the namespace body exited rc=$rc"; exit 1; }

echo "$OUT" | sed 's/^/  /'

fail() { echo "FAIL: $1"; exit 1; }
line() { echo "$OUT" | grep -a "^$1 " | head -1; }
st()   { line "$1" | awk '{print $2}'; }
upness() { line "$1" | awk '{print $3}'; }
alias_of() { line "$1" | sed -n 's/.*alias=\(.*\)$/\1/p'; }

echo "$OUT" | grep -aq '^a-daemon-died' && fail "the daemon died at start"

# 0. THE INSTRUMENT WORKS IN EVERY DIRECTION IT IS ABOUT TO BE TRUSTED IN.
[ "$(st control-present)" = "present" ] \
	|| fail "the reader could not SEE a netdev that had just been created — every absence below would be a broken search"
[ "$(st control-absent)" = "absent" ] \
	|| fail "the reader reported a netdev after it was deleted"
[ "$(alias_of control-present)" = "reac-pw:minted" ] \
	|| fail "the reader could not see an alias that had just been SET: $(line control-present)"
[ "$(alias_of control-unmarked)" = "none" ] \
	|| fail "the reader invented an alias on a netdev that carries none: $(line control-unmarked)"

# 2. THE POSITIVE CONTROL FIRST: the tag was heard and the declared VID was minted on it.
#    Without this, "absent" in silence is a detector that never mints anything.
[ "$(st a-heard-11)" = "1" ] \
	|| fail "tagged frames inside VID 11 were never heard on trunkA — the injection did not land, so nothing here is a measurement"
[ "$(st a-tagged-11)" = "present" ] \
	|| fail "VID 11 was heard on trunkA and the DECLARED trunkA.11 was not minted: $(line a-tagged-11)"
[ "$(upness a-tagged-11)" = "up" ] \
	|| fail "trunkA.11 was minted and left DOWN: $(line a-tagged-11)"
[ "$(alias_of a-tagged-11)" = "reac-pw:minted" ] \
	|| fail "trunkA.11 carries no mint alias, so the next start would inherit it as the host's: $(line a-tagged-11)"
[ "$(st a-role-11)" = "master" ] \
	|| fail "trunkA.11 was minted on its tag and the declared role=master was not reported on it"

# 1. SILENCE MINTS NOTHING — the ruling. Red on 1623184, which minted both at start.
[ "$(st a-before-11)" = "absent" ] || fail "trunkA.11 existed before the daemon ran: $(line a-before-11)"
[ "$(st a-silent-11)" = "absent" ] \
	|| fail "trunkA.11 is declared, nothing was heard, and it was minted anyway — 'we don't carry any VLANs if we don't detect VLANs': $(line a-silent-11)"
[ "$(st a-silent-33)" = "absent" ] \
	|| fail "trunkA.33 (a bare [segment] section) was minted in silence: $(line a-silent-33)"
[ "$(st a-tagged-33)" = "absent" ] \
	|| fail "VID 11 was heard and trunkA.33 — a VID nobody tagged — was minted with it: $(line a-tagged-33)"
echo "$OUT" | grep -aq 'a: .*none is minted until a tagged frame' \
	|| fail "the daemon never said that declared segments wait for their tag"

# 1b. THE HOST'S OWN SUB-INTERFACE IS UNTOUCHED, THROUGHOUT.
[ "$(st a-before-22)" = "present" ] || fail "the host-made control was not set up"
[ "$(st a-silent-22)" = "present" ] || fail "the daemon removed a netdev the host made"
[ "$(alias_of a-silent-22)" = "none" ] \
	|| fail "the daemon marked a netdev the HOST created as its own: $(line a-silent-22)"

# 3. THE EXIT. What it minted is gone; what the host made is untouched.
[ "$(st a-after-11)" = "absent" ] \
	|| fail "the daemon exited and left the netdev it minted behind: $(line a-after-11)"
[ "$(st a-after-22)" = "present" ] \
	|| fail "the daemon removed a netdev the HOST created — it was never ours to take: $(line a-after-22)"

echo "OK"
exit 0
