#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE CLASSIFIER READS THE KERNEL, NOT A GUESS (#46,
# 2026-10-26-a-host-nic-is-never-bounced-or-streamed-at). On the desk the knock streamed
# at, and the wake ladder bounced, the trunk parent that carries the management address
# and every REAC VLAN. reac_link_owner says which interfaces are the host's, from three
# rtnetlink dumps. This drives the probe over a veth in a private namespace and changes
# ONE fact at a time:
#
#   bare       a veth with nothing on it (its peer names it in IFLA_LINK: not stacked)
#   link-only  169.254/16 scope link and the kernel's fe80: still bare (NetworkManager puts
#              both on a NIC it manages, and a REAC wire carries them too)
#   addr       10.99.1.1/24: the host's
#   stacked    a VLAN child: the host's
#   enslaved   a bridge port (IFLA_MASTER on the NIC itself): the host's, as stacked
#   route      a default route out of it, no address: the host's
#   unread     a name that does not exist: unread, with ENODEV
#
# Skips (77) where unprivileged user+net namespaces or iproute2 are unavailable.
set -u
PROBE="${1:?usage: $0 /path/to/link_owner_probe}"
SKIP=77
for t in unshare ip; do
	command -v $t >/dev/null 2>&1 || { echo "SKIP: no $t"; exit $SKIP; }
done
unshare -r -n true 2>/dev/null || { echo "SKIP: unprivileged user+net namespaces unavailable"; exit $SKIP; }

unshare -r -n bash -s -- "$PROBE" <<'INNER'
set -u
PROBE="$1"
FAIL=0
want() {   # want <case> <verdict-word>
	local got
	got=$("$PROBE" lo0x 2>&1)
	case "$got" in "$2 "*) echo "ok   $1: $got";; *) echo "FAIL $1: wanted '$2', got '$got'"; FAIL=1;; esac
}
ip link add lo0x type veth peer name lo0y || { echo "SKIP: cannot add a veth pair here"; exit 77; }
ip link set lo0x up; ip link set lo0y up
sleep 0.3                                            # fe80 is added as the link comes up
want bare bare
ip addr add 169.254.7.7/16 scope link dev lo0x
want link-only bare
ip addr add 10.99.1.1/24 dev lo0x
want addr addr
ip addr del 10.99.1.1/24 dev lo0x
want addr-removed bare
ip link add link lo0x name lo0x.11 type vlan id 11
want stacked stacked
ip link del lo0x.11
want child-removed bare
ip link add br0x type bridge
ip link set lo0x master br0x
want enslaved stacked
ip link set lo0x nomaster
want unenslaved bare
ip link del br0x
ip route add default dev lo0x
want route route
ip route del default dev lo0x
want route-removed bare
got=$("$PROBE" no-such-if0 2>&1)
case "$got" in "unread "*"err=-19"*) echo "ok   unread: $got";; *) echo "FAIL unread: got '$got'"; FAIL=1;; esac
# and the peer, which names lo0x in its own IFLA_LINK, is bare too
got=$("$PROBE" lo0y 2>&1)
case "$got" in "bare "*) echo "ok   peer: $got";; *) echo "FAIL peer: got '$got'"; FAIL=1;; esac
[ "$FAIL" = 0 ] && echo "PASS: bare, link-scope-only and the peer read bare; an address, a VLAN child, a bridge enslaving it and a default route each read as the host's; a missing name reads unread"
exit $FAIL
INNER
