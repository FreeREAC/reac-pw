#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE ONE PLACE A HARNESS IS BUILT (fuzz/Makefile and fuzz/records.sh call it). A harness
# links its own file, the reac-pw sources of the reader it drives, and nothing of libreac
# but its headers (the box-model table is fuzz/catalogue.c, the netlink socket the
# harness's own). The sources are compiled as meson compiles them, C11 with _GNU_SOURCE.
#
#   fuzz/build.sh <variant> <harness> <output>
#
#   san     libFuzzer, AddressSanitizer and UndefinedBehaviorSanitizer
#   cov     libFuzzer, no sanitizer, instrumented for llvm-cov
#   plain   no sanitizer, fuzz/standalone.c for its driver; FUZZ_EXTRA is added to the
#           harness's own compile only (fuzz/records.sh names each abort() through it)
#   static  as plain, optimised, and linked fully static
#
# FUZZ_CC names the compiler (clang); FUZZ_DEPS the include flags of the libreac and SPA
# headers (pkg-config's, by default).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
variant=$1 h=$2 out=$3
CC=${FUZZ_CC:-clang}
DEPS=${FUZZ_DEPS:-$(pkg-config --cflags libreac libreac-transport libspa-0.2)}
EXTRA=${FUZZ_EXTRA:-}

# The reac-pw sources each harness drives.
case $h in
segconf) srcs="src/reac_segconf.c src/reac_declared_vlan.c" ;;
pin) srcs="src/reac_box_pin.c" ;;
headamp) srcs="src/reac_headamp_prop.c" ;;
rate) srcs="src/reac_rate_cfg.c" ;;
role) srcs="src/reac_role_cfg.c" ;;
qdisc) srcs="src/reac_qdisc.c" ;;
link) srcs="src/reac_link_alias.c src/reac_served_mark.c" ;;
*) echo "fuzz/build.sh: no harness '$h'" >&2; exit 2 ;;
esac
# The harness's own helpers beside it.
case $h in
segconf | pin) helpers="fuzz/catalogue.c" ;;
*) helpers= ;;
esac

# THE NETLINK SOCKET IS THE HARNESS'S. The two readers of rtnetlink replies are compiled
# with their socket calls renamed to the harness's fake kernel (fuzz/netlink.h), which
# hands them the reply bytes of the input. Renaming at the compile is the way that works
# under every OSS Verify build (its docs/build-contract.md, "Functions that the
# AddressSanitizer runtime wraps"); fortification would put libc's own inline recv back.
NET="-U_FORTIFY_SOURCE -Dsocket=fuzz_socket -Dsend=fuzz_send -Drecv=fuzz_recv -Dclose=fuzz_close \
-Dif_nametoindex=fuzz_if_nametoindex"
own_flags() {
	case $1 in
	src/reac_qdisc.c | src/reac_link_alias.c) echo "$NET" ;;
	esac
}

STD="-std=c11 -D_GNU_SOURCE -Wall -Wextra -Isrc -Ifacts -Ifuzz $DEPS"
LDX=

case $variant in
san)
	opt="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all"
	lib="$opt -fsanitize=fuzzer-no-link"
	link="$opt -fsanitize=fuzzer"
	driver= ;;
cov)
	opt="-O0 -g -fprofile-instr-generate -fcoverage-mapping"
	lib=$opt
	link="$opt -fsanitize=fuzzer"
	driver= ;;
plain)
	opt="-O0 -g"
	lib=$opt
	link=$opt
	driver=fuzz/standalone.c ;;
static)
	opt="-O2 -g"
	lib=$opt
	link="$opt -static"
	driver=fuzz/standalone.c ;;
*) echo "fuzz/build.sh: no variant '$variant'" >&2; exit 2 ;;
esac

obj=$out.obj
rm -rf "$obj"
mkdir -p "$obj"
objs=
for s in $srcs; do
	o=$obj/$(basename "$s" .c).o
	# shellcheck disable=SC2046
	$CC $STD $lib $(own_flags "$s") -c "$s" -o "$o"
	objs="$objs $o"
done
for s in $helpers; do
	o=$obj/$(basename "$s" .c).o
	$CC $STD $lib -c "$s" -o "$o"
	objs="$objs $o"
done
for s in $driver; do
	o=$obj/$(basename "$s" .c).o
	$CC $STD $opt -c "$s" -o "$o"
	objs="$objs $o"
done
$CC $STD $lib $EXTRA -c "fuzz/fuzz_$h.c" -o "$obj/fuzz_$h.o"
# shellcheck disable=SC2086
$CC $link $LDX "$obj/fuzz_$h.o" $objs -lm -o "$out"
