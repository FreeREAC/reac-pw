#!/bin/bash
# The documented build path for reac-pw. Use this, not a bare `meson compile`.
#
# FILE CAPABILITIES LIVE ON THE INODE, AND EVERY RELINK MAKES A NEW ONE. So each
# `meson compile` silently drops them, and a reac-pw without cap_net_raw cannot open
# its raw socket. Before the startup refusal landed beside this script, that state
# looked entirely healthy — PipeWire nodes appeared, the log read normally, and no
# box ever synced. A whole evening went into "reac-pw is not synching boxes" that
# was only ever a missing capability on a fresh build.
#
# The VERIFY step is the half that matters: getcap coming back empty is a hard
# failure here, never a warning. A build that silently produces an uncapable
# binary is the same false-signal class as a green suite over a dead path.
set -o pipefail
builddir="${1:-build}"
bin="$builddir/reac-pw"

meson compile -C "$builddir" || exit 1

sudo -n setcap cap_net_raw,cap_net_admin,cap_sys_nice=ep "$bin" || {
	echo "build.sh: setcap FAILED on $bin — the binary cannot open a raw socket" >&2
	exit 1
}

# getcap NORMALISES the capability order alphabetically, so a fixed-order glob is
# a false failure waiting to happen. Check each one independently.
caps=$(getcap "$bin")
missing=""
for c in cap_net_raw cap_net_admin cap_sys_nice; do
	case "$caps" in *"$c"*) ;; *) missing="$missing $c" ;; esac
done
case "$caps" in *=ep*) ;; *) missing="$missing (effective+permitted flags)" ;; esac

if [ -n "$missing" ]; then
	echo "build.sh: VERIFY FAILED — $bin is missing:$missing" >&2
	echo "          want: cap_net_raw,cap_net_admin,cap_sys_nice=ep" >&2
	echo "          getcap said: '${caps:-<empty>}'" >&2
	echo "          A filesystem mounted nosuid drops capabilities SILENTLY —" >&2
	echo "          setcap reports success and changes nothing. A build tree" >&2
	echo "          under /tmp is the usual cause; build on the home fs." >&2
	exit 1
fi
echo "build.sh: $caps"
