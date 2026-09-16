#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# WHOLE-BINARY: a DECLARED VLAN segment is minted before anything is heard on it.
#
# THE FAULT, MEASURED ON THE DESK 2026-09-15 AT A REBOOT. reac-pw minted
# `<parent>.<vid>` only when it HEARD a tagged REAC frame on the trunk. On a cold boot no
# such frame can arrive: every box on the trunk is a SLAVE, a slave says nothing until a
# master speaks, and the master cannot speak until its segment's netdev exists. Nothing
# heard, nothing minted, nothing to hear. Every declared segment came up dead, every box
# unenrolled, and no error was printed anywhere — the daemon did exactly what it said.
#
# WHAT A UNIT TEST CANNOT SEE HERE. The key-parsing rules are pinned in
# tests/test_reac_declared_vlan.c, and every one of them can be right while the daemon
# still never calls the minting path — which is precisely the shape this repo keeps
# paying for. So this asks the KERNEL what netdevs exist, around the real binary:
#
#   1. DECLARED, PARENT PRESENT, NETDEV ABSENT. The segment is declared in the daemon's
#      own conf file and nothing is ever transmitted on the wire. After the start the
#      netdev must be there, UP, and carrying the mint alias.
#   2. DECLARED, PARENT ABSENT AT START. The daemon starts before the parent exists (the
#      NetworkManager race). The netdev must appear when the parent does, not before and
#      not never.
#   3. EXIT. What it minted is gone; what the host made is untouched. The adopted arm is
#      the control: without it "gone" could just mean the daemon deletes VLANs.
#
# THE INSTRUMENT IS PROVEN BEFORE ANY ABSENCE IS BELIEVED: a netdev is created by hand and
# the same reader that will report absences is required to SEE it first.
#
# ISOLATION, both halves (tests/etf-qdisc-owned.sh, same reason): a user+net+pid namespace
# with its own veth AND its own PipeWire on a private runtime dir, because `unshare -n`
# isolates the wire and not the graph. HOME is redirected at the daemon so the conf file
# this test writes is the only one it reads — never the operator's.
set -u
BIN="${1:?usage: $0 /path/to/reac-pw}"
SKIP=77

for t in unshare ip pipewire pw-cli; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
unshare -r -n -p -f --mount-proc --map-root-user true 2>/dev/null || {
	echo "SKIP: unprivileged user+net+pid namespaces unavailable"; exit $SKIP; }

OUT=$(unshare -r -n -p -f --mount-proc --map-root-user bash -s -- "$BIN" <<'INNER'
set -u
BIN="$1"
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

# THE DAEMON'S OWN HOME, so the declarations below are the only ones it can read.
CONF="$RT/home"
mkdir -p "$CONF/.config/reac-pw"

# What is on this host, as the kernel answers it over netlink: `present`/`absent`, the
# operational flag, and the interface ALIAS, which is the mint mark reac_vlan writes.
#
# THE ALIAS IS READ FROM `ip`, NOT FROM /sys. This namespace inherits the HOST'S sysfs
# (only /proc is remounted), so `/sys/class/net/<if>/ifalias` does not exist for a netdev
# that lives in here — and cat'ing a missing file reports "no alias" for every netdev
# alike, which is a reader that cannot tell a marked netdev from an unmarked one. Met
# here once: the mint arm read alias=none while the daemon's own line said it had marked
# it. Hence the control below, which requires the reader to SEE an alias it just set.
netdev() {
	if ! ip -o link show "$1" >/dev/null 2>&1; then echo "absent"; return; fi
	local up al
	up=$(ip -o link show "$1" | grep -qw UP && echo up || echo down)
	al=$(ip -d -o link show "$1" 2>/dev/null | tr '\\' '\n' \
	     | sed -n 's/^ *alias \(.*\)$/\1/p' | head -1)
	echo "present $up alias=${al:-none}"
}

# THE READER MUST BE ABLE TO SEE A NETDEV, AND AN ALIAS ON IT, BEFORE ANY ABSENCE IT
# REPORTS MEANS ANYTHING. Both halves: a marked netdev, an unmarked one, and none at all.
ip link add ctrl0 type dummy 2>/dev/null || ip link add ctrl0 type veth peer name ctrl1
echo "control-unmarked $(netdev ctrl0)"
ip link set dev ctrl0 alias reac-pw:minted
echo "control-present $(netdev ctrl0)"
ip link del ctrl0 2>/dev/null
echo "control-absent $(netdev ctrl0)"

# ---- 1 + 3. PARENT PRESENT, DECLARED NETDEV ABSENT, plus an ADOPTED control ---------
ip link add trunkA type veth peer name farA || exit 90
ip link set trunkA up; ip link set farA up
# The adopted control: the HOST made this one, so the exit must leave it behind.
ip link add link trunkA name trunkA.22 type vlan id 22 || exit 90
ip link set trunkA.22 up

cat > "$CONF/.config/reac-pw/reac-pw.conf" <<EOF
[segment trunkA.11]
role = master

[segment trunkA.22]
role = master

# A SECTION THAT SAYS NOTHING IS STILL A DECLARATION. Naming the segment is the whole act;
# role and ignore are separate questions about it.
[segment trunkA.33]
EOF

echo "a-before-11 $(netdev trunkA.11)"
echo "a-before-22 $(netdev trunkA.22)"
echo "a-before-33 $(netdev trunkA.33)"

HOME="$CONF" "$BIN" >"$RT/a.log" 2>&1 &
APID=$!
sleep 4
kill -0 $APID 2>/dev/null || { echo "a-daemon-died"; tail -5 "$RT/a.log"; exit 1; }
# NOTHING HAS EVER BEEN TRANSMITTED ON THIS WIRE. That is the whole point: no master
# speaks, no tagged frame is heard, and the netdev has to be there anyway.
echo "a-running-11 $(netdev trunkA.11)"
echo "a-running-22 $(netdev trunkA.22)"
echo "a-running-33 $(netdev trunkA.33)"
kill -TERM $APID 2>/dev/null
for i in $(seq 40); do kill -0 $APID 2>/dev/null || break; sleep 0.2; done
kill -9 $APID 2>/dev/null; wait $APID 2>/dev/null
echo "a-after-11 $(netdev trunkA.11)"
echo "a-after-22 $(netdev trunkA.22)"
echo "a-after-33 $(netdev trunkA.33)"
grep -aE 'declared' "$RT/a.log" | tr -d '\r' | sed 's/^/  a: /' | head -8

# ---- 2. THE PARENT IS NOT THERE YET (the NetworkManager race) -----------------------
cat > "$CONF/.config/reac-pw/reac-pw.conf" <<EOF
[segment trunkB.11]
role = master
EOF
echo "b-before-parent $(netdev trunkB)"
echo "b-before-11 $(netdev trunkB.11)"
HOME="$CONF" "$BIN" >"$RT/b.log" 2>&1 &
BPID=$!
sleep 3
kill -0 $BPID 2>/dev/null || { echo "b-daemon-died"; tail -5 "$RT/b.log"; exit 1; }
echo "b-noparent-11 $(netdev trunkB.11)"
# THE PARENT APPEARS. This is the RTM_NEWLINK the daemon must act on.
ip link add trunkB type veth peer name farB || exit 90
ip link set trunkB up; ip link set farB up
sleep 3
echo "b-parent-11 $(netdev trunkB.11)"
kill -TERM $BPID 2>/dev/null
for i in $(seq 40); do kill -0 $BPID 2>/dev/null || break; sleep 0.2; done
kill -9 $BPID 2>/dev/null; wait $BPID 2>/dev/null
echo "b-after-11 $(netdev trunkB.11)"
grep -aE 'declared' "$RT/b.log" | tr -d '\r' | sed 's/^/  b: /' | head -6
exit 0
INNER
)
rc=$?
[ $rc -eq 0 ] || { echo "SKIP: the namespace body could not run (rc=$rc)"
                   echo "$OUT" | sed 's/^/  /'; exit $SKIP; }

echo "$OUT" | sed 's/^/  /'

fail() { echo "FAIL: $1"; exit 1; }
line() { echo "$OUT" | grep -a "^$1 " | head -1; }
st()   { line "$1" | awk '{print $2}'; }
upness() { line "$1" | awk '{print $3}'; }
alias_of() { line "$1" | sed -n 's/.*alias=\(.*\)$/\1/p'; }

echo "$OUT" | grep -aq '^a-daemon-died' && fail "the parent-present arm's daemon died at start"
echo "$OUT" | grep -aq '^b-daemon-died' && fail "the late-parent arm's daemon died at start"

# 0. THE INSTRUMENT WORKS IN EVERY DIRECTION IT IS ABOUT TO BE TRUSTED IN.
[ "$(st control-present)" = "present" ] \
	|| fail "the reader could not SEE a netdev that had just been created — every absence below would be a broken search"
[ "$(st control-absent)" = "absent" ] \
	|| fail "the reader reported a netdev after it was deleted"
[ "$(alias_of control-present)" = "reac-pw:minted" ] \
	|| fail "the reader could not see an alias that had just been SET — every 'no alias' below would be a broken search: $(line control-present)"
[ "$(alias_of control-unmarked)" = "none" ] \
	|| fail "the reader invented an alias on a netdev that carries none: $(line control-unmarked)"

# 1. DECLARED AND ABSENT -> MINTED, UP, MARKED, with nothing ever heard on the wire.
[ "$(st a-before-11)" = "absent" ] || fail "trunkA.11 existed before the daemon ran: $(line a-before-11)"
[ "$(st a-running-11)" = "present" ] \
	|| fail "trunkA.11 is DECLARED and was never minted — this is the cold-boot fault: $(line a-running-11)"
[ "$(upness a-running-11)" = "up" ] \
	|| fail "trunkA.11 was minted and left DOWN, which looks exactly like a box that is not talking: $(line a-running-11)"
[ "$(alias_of a-running-11)" = "reac-pw:minted" ] \
	|| fail "trunkA.11 carries no mint alias, so the next start would inherit it as the host's: $(line a-running-11)"

# 1b. A SECTION WITH NO KEYS IS STILL A DECLARATION.
[ "$(st a-before-33)" = "absent" ] || fail "trunkA.33 existed before the daemon ran"
[ "$(st a-running-33)" = "present" ] \
	|| fail "trunkA.33 is declared by a bare [segment] section and was never minted: $(line a-running-33)"

# 2. THE ADOPTED CONTROL. The host made trunkA.22; the daemon serves it and never owns it.
[ "$(st a-before-22)" = "present" ] || fail "the adopted control was not set up"
[ "$(st a-running-22)" = "present" ] || fail "the daemon removed a netdev the host made"
[ "$(alias_of a-running-22)" = "none" ] \
	|| fail "the daemon marked a netdev the HOST created as its own: $(line a-running-22)"

# 3. THE EXIT. What it minted is gone; what it adopted is untouched. Both halves, or
#    "gone" would only mean the daemon deletes VLAN netdevs.
[ "$(st a-after-11)" = "absent" ] \
	|| fail "the daemon exited and left the netdev it minted behind: $(line a-after-11)"
[ "$(st a-after-33)" = "absent" ] \
	|| fail "the daemon exited and left trunkA.33 behind: $(line a-after-33)"
[ "$(st a-after-22)" = "present" ] \
	|| fail "the daemon removed a netdev the HOST created — it was never ours to take: $(line a-after-22)"

# 4. THE PARENT ARRIVES LATE. The daemon may start before NetworkManager brings the trunk
#    up, and a declaration that is only honoured at second zero is a declaration that a
#    boot order can silently defeat.
[ "$(st b-before-parent)" = "absent" ] || fail "trunkB existed before the late-parent arm"
[ "$(st b-noparent-11)" = "absent" ] \
	|| fail "trunkB.11 appeared with no parent to hang it on: $(line b-noparent-11)"
[ "$(st b-parent-11)" = "present" ] \
	|| fail "the parent appeared and the DECLARED segment was not minted on it: $(line b-parent-11)"
[ "$(upness b-parent-11)" = "up" ] \
	|| fail "trunkB.11 was minted on the late parent and left DOWN: $(line b-parent-11)"
[ "$(st b-after-11)" = "absent" ] \
	|| fail "the late-parent arm left its mint behind: $(line b-after-11)"

echo "OK"
exit 0
