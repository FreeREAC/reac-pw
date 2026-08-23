#!/bin/bash
# One A/B leg on the S-0808 segment. Never touches enp131s0 (the S-1608, whose
# 16 capture channels are linked to the console and may carry the live mic).
set -o pipefail
S=/tmp/claude-1000/-home-pau-Devel-audio-openmixer/7d6d683f-e40e-4ae4-af00-ee77ed53dc70/scratchpad
BIN=${BIN:-/home/pau/Devel/audio/reac-pw/build/reac-pw}
TAG=$1; DUR=${2:-240}
export XDG_RUNTIME_DIR=/run/user/1000

# --- SAFETY: exactly two masters, and the one we replace is the S-0808's.
n=$(pgrep -x reac-pw | wc -l)
[ "$n" = 2 ] || { echo "REFUSING: $n reac-pw processes, expected 2 — a restart may be in flight"; exit 1; }
old=$(pgrep -x reac-pw | while read p; do tr '\0' ' ' </proc/$p/cmdline | grep -q enp128s20f0u6 && echo $p; done)
[ "$(echo "$old" | wc -w)" = 1 ] || { echo "REFUSING: $(echo "$old"|wc -w) masters on enp128s20f0u6"; exit 1; }
grep -q "^Name:.reac-pw$" /proc/$old/status || { echo "REFUSING: pid $old is not reac-pw"; exit 1; }
grep -q "^Tgid:.$old$" /proc/$old/status  || { echo "REFUSING: pid $old is not a thread group leader"; exit 1; }

echo "== replacing pid $old with $TAG"
kill -TERM "$old"
for i in $(seq 1 40); do kill -0 "$old" 2>/dev/null || break; sleep 0.25; done
kill -0 "$old" 2>/dev/null && { echo "REFUSING to continue: pid $old still alive"; exit 1; }
echo "   old master gone"

setsid nohup "$BIN" --live enp128s20f0u6 --tx enp128s20f0u6 --mixer m200 --rate 48000 \
   > "$S/$TAG.log" 2>&1 &
sleep 4
new=$(pgrep -x reac-pw | while read p; do tr '\0' ' ' </proc/$p/cmdline | grep -q enp128s20f0u6 && echo $p; done)
[ -n "$new" ] || { echo "FAILED: no master on enp128s20f0u6"; tail -20 "$S/$TAG.log"; exit 1; }
echo "   new pid $new  exe=$(sudo -n readlink /proc/$new/exe)"
# VERIFY WHAT WE ARE MEASURING IS WHAT WE BUILT: only this build emits reac-health.
grep -q "another openmixer engine owns\|REFUSING to master" "$S/$TAG.log" && { echo "FAILED: segment lock refused"; exit 1; }

# settle, then measure a clean window
sleep 60
python3 - "$DUR" <<'PY' > "$S/$TAG.wire"
import sys,time
d=float(sys.argv[1])
def snap():
    b='/sys/class/net/enp128s20f0u6/statistics/'
    t=time.clock_gettime(time.CLOCK_MONOTONIC)
    return t,int(open(b+'tx_packets').read()),int(open(b+'rx_packets').read())
t0,a,ra=snap(); time.sleep(d); t1,b,rb=snap()
dt=t1-t0
print(f"wire window {dt:.3f}s TX {(b-a)/dt:.4f} pps ({(((b-a)/dt)/4000-1)*1e6:+.1f} ppm)  RX {(rb-ra)/dt:.4f} pps ({(((rb-ra)/dt)/4000-1)*1e6:+.1f} ppm)")
PY
echo "== $TAG done"; cat "$S/$TAG.wire"
