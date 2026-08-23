#!/bin/bash
# The documented build path for reac-pw. Use this, not a bare `meson compile`.
#
# FILE CAPABILITIES LIVE ON THE INODE, AND EVERY RELINK MAKES A NEW ONE. So each
# `meson compile` silently drops cap_net_raw, and a reac-pw without it cannot open
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

sudo -n setcap cap_net_raw,cap_sys_nice=ep "$bin" || {
	echo "build.sh: setcap FAILED on $bin — the binary cannot open a raw socket" >&2
	exit 1
}

caps=$(getcap "$bin")
case "$caps" in
	*cap_net_raw*cap_sys_nice*)
		echo "build.sh: $caps"
		;;
	*)
		echo "build.sh: VERIFY FAILED — $bin has no capabilities after setcap." >&2
		echo "          getcap said: '${caps:-<empty>}'" >&2
		echo "          A filesystem mounted nosuid drops them silently; a build" >&2
		echo "          tree under /tmp is the usual cause. Build on the home fs." >&2
		exit 1
		;;
esac
