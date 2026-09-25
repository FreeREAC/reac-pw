#!/usr/bin/env python3
"""Is the AUDIO region of our downstream frames actually carrying signal?

A REAC downstream frame is 1492 B: header, control block [18:50], then audio.
A master whose sink has stopped feeding it still emits perfectly good frames at
exactly the right rate with a zero audio region — which is why "frames are going
out" and "audio is reaching the box" are different questions, and only this one
matters. Reports, per 1-second bucket, how many frames carry a non-zero audio
region so a path that dies partway through is visible as a cliff rather than an
average.

  tools/wire-audio.py capture.pcap <src-mac>
"""
import struct, sys, collections
import os as _os
sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from facts import FACTS   # the protocol's numbers, from their one declaration (tools/facts.py)

path = sys.argv[1]
want = bytes.fromhex(sys.argv[2].replace(':', ''))
f = open(path, 'rb')
magic, = struct.unpack('<I', f.read(24)[:4])
en = '<' if magic in (0xa1b2c3d4, 0xa1b23c4d) else '>'
nano = magic == 0xa1b23c4d

live = collections.Counter(); total = collections.Counter(); t0 = None
# A LEAKED-BUFFER PATH DOES NOT GO TO ZERO, IT GOES STATIC: the same PCM is
# re-encoded every cycle. "non-zero" cannot tell those apart, so count how many
# frames differ from the one before — a live signal changes, a stale one does not.
changed = collections.Counter(); prev = None
while True:
    rh = f.read(16)
    if len(rh) < 16:
        break
    ts, tus, incl, _ = struct.unpack(en + 'IIII', rh)
    pkt = f.read(incl)
    if len(pkt) < incl:
        break
    t = ts + (tus / 1e9 if nano else tus / 1e6)
    if t0 is None:
        t0 = t
    if pkt[6:12] != want or len(pkt) < FACTS["FRAME_BYTES"]:
        continue
    b = int(t - t0)
    total[b] += 1
    # AUDIO_OFFSET up to the end marker: this sliced [50:1492] until 2026-09-25 and so
    # took in the two end-marker bytes (audit contract-copies, wire-audio.py:43).
    aud = pkt[FACTS["AUDIO_OFFSET"]:FACTS["FRAME_BYTES"] - FACTS["END_MARKER_BYTES"]]
    if any(aud):
        live[b] += 1
    if prev is not None and aud != prev:
        changed[b] += 1
    prev = aud

if not total:
    sys.exit('NO FRAMES from that MAC — wrong filter or wrong capture. '
             'Prove the reader sees frames before believing a silent result.')
print(f'{"sec":>4} {"frames":>7} {"audio!=0":>9} {"changed":>8}')
for b in sorted(total):
    print(f'{b:>4} {total[b]:>7} {live[b]:>9} {changed[b]:>8}')
n, l, c = sum(total.values()), sum(live.values()), sum(changed.values())
print(f'\ntotal {l}/{n} carry audio ({100.0*l/n:.1f}%), '
      f'{c}/{n} differ from the previous frame ({100.0*c/n:.1f}%)')
if l and not c:
    print('STATIC: every frame identical — the encoder is re-sending one buffer.')
