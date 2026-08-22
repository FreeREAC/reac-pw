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
import struct, sys

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
        i = pkt.find(b'\xcd\xea')
        if i < 0:
            continue
        blk = pkt[i:i + 34]
        op = blk[2:4]
        if op not in (b'\x01\x01', b'\x01\x00', b'\x01\x02') or blk == prev:
            continue
        prev = blk
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
