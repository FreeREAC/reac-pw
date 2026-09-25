#!/usr/bin/env python3
"""Recover a desk's 8904-byte scene body from a REAC establish capture.

The master pushes the scene as one op-0101 header carrying the total, 341 op-0100
chunks of 26 bytes, and one op-0102 final chunk of 14 — 24 + 341*26 + 14 = 8904 =
0x22c8, the constant the S-1608 checks its header against. The total is the
self-check: a run that does not reach 8904 recovered a truncated or decimated
transfer and must not be used.

A mirrored capture repeats every frame; consecutive identical control blocks are
dropped. The 7-of-8 frame loss that mirrors inflict on AUDIO does not touch these
control frames — 2729 op-0100 in the 2026-07-11 M-200i capture is 341 x 8 transfers.
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from facts import FACTS   # the protocol's numbers, from their one declaration (tools/facts.py)

def recover(path):
    f = open(path, 'rb')
    magic, = struct.unpack('<I', f.read(24)[:4])
    endian = '<' if magic in (0xa1b2c3d4, 0xa1b23c4d) else '>'
    scene, state, chunks, prev, total = b'', 0, 0, None, None
    while True:
        rh = f.read(16)
        if len(rh) < 16:
            break
        _, _, incl, _ = struct.unpack(endian + 'IIII', rh)
        pkt = f.read(incl)
        if len(pkt) < incl:
            break
        # Mirror dedup by GEOMETRY, not by comparing bytes: reac.ksy fixes a real
        # frame at 52 + n*36, and a switch mirror hands over the same frame again
        # with two bytes of the capture's own FCS left on the end. The residue copy
        # is the one whose length is not 52 + n*36, so it is decidable rather than
        # heuristic -- a content compare would also drop a frame a desk repeated on
        # purpose.
        ovh, per = FACTS["FRAME_OVERHEAD"], FACTS["BYTES_PER_CHANNEL"]
        if len(pkt) < ovh or (len(pkt) - ovh) % per != 0:
            continue
        i = pkt.find(b'\xcd\xea')
        if i < 0:
            continue
        blk = pkt[i:i + 34]
        op = blk[2:4]
        if op not in (b'\x01\x01', b'\x01\x00', b'\x01\x02'):
            continue
        if op == b'\x01\x01':
            total, = struct.unpack('>H', blk[7:9])
            scene, state, chunks = blk[9:9 + 24], 1, 0
        elif op == b'\x01\x00' and state == 1:
            scene += blk[7:7 + 26]
            chunks += 1
        elif op == b'\x01\x02' and state == 1:
            scene += blk[7:7 + 14]
            break
    return scene, chunks, total

if __name__ == '__main__':
    scene, chunks, total = recover(sys.argv[1])
    print(f'declared {total} chunks {chunks} recovered {len(scene)}')
    if len(scene) != 8904:
        sys.exit('REFUSED: not 8904 bytes — truncated or decimated capture')
    if len(sys.argv) > 2:
        open(sys.argv[2], 'wb').write(scene)

# Cross-desk matrix, 2026-08-23 (one capture per combination, self-checked at 8904):
#
#   desk    x box      declared  chunks  bytes  scene[+4]  sha256[:12]
#   M-5000  x S-0808     8904      341    8904    01 00    efc316a55b00
#   M-300   x S-1608     8904      341    8904    01 00    89947703badd
#   M-300   x S-0808     8904      341    8904    01 00    89947703badd
#   M-200i  x S-1608     8904      341    8904    01 00    efc316a55b00
#
# Two readings, both load-bearing:
#   THE SCENE IS A PROPERTY OF THE DESK, NOT THE BOX. M-300 sends one scene to an S-1608
#   and the same bytes to an S-0808; M-200i and M-5000 send a different scene, byte
#   identical to each other. Nothing about it is sized or keyed to the box.
#   SCENE[+4] IS 01 00 ON EVERY DESK GENERATION AND EVERY BOX. A master that drives both
#   banks sends 01 there, so zeroing it moves away from working hardware, not toward it.
