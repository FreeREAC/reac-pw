#!/usr/bin/env python3
"""Report the scene transfers in a CLEAN (non-mirrored) capture of one master.

recover-scene.py drops consecutive identical control blocks, which is right for a
MIRRORED capture (every frame appears twice) and wrong for a clean one, where a
repeated block is something the master really emitted twice. This tool never
dedups: it walks the frames in order and reports each header -> chunks -> final
run exactly as it went out, so "how many chunks did we actually put on the wire"
is answerable separately from "how many distinct ones".

A transfer is COMPLETE when a header is followed by a final and the payload bytes
between them sum to the total the header declared.

  tools/scene-on-wire.py capture.pcap [src-mac]
"""
import os, struct, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from facts import FACTS   # the protocol's numbers, from their one declaration (tools/facts.py)

CTRL, TL = FACTS["TYPE_CONTROL"].to_bytes(2, 'big'), FACTS["TYPED_BLOCK_LEN"]

HEAD, CHUNK, FINAL = FACTS["OP_SCENE_HEADER"], FACTS["OP_SCENE_CHUNK"], FACTS["OP_SCENE_FINAL"]
HEAD_B, CHUNK_B, FINAL_B = FACTS["SCENE_HEAD_BYTES"], FACTS["SCENE_CHUNK_BYTES"], FACTS["SCENE_TAIL_BYTES"]
TW = FACTS["TYPE_WORD_BYTES"]
OP, TOT = TW + FACTS["SCENE_OP_OFF"], TW + FACTS["SCENE_HEAD_TOTAL_OFF"]
HPAY, CPAY = TW + FACTS["SCENE_HEAD_PAY_OFF"], TW + FACTS["SCENE_CHUNK_PAY_OFF"]
NCHUNKS = FACTS["SCENE_CHUNKS"]

path = sys.argv[1]
want = bytes.fromhex(sys.argv[2].replace(':', '')) if len(sys.argv) > 2 else None

f = open(path, 'rb')
magic, = struct.unpack('<I', f.read(24)[:4])
en = '<' if magic in (0xa1b2c3d4, 0xa1b23c4d) else '>'
nano = magic == 0xa1b23c4d

events, t0 = [], None
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
    if want and pkt[6:12] != want:
        continue
    i = pkt.find(CTRL)
    if i < 0:
        continue
    blk = pkt[i:i + TL]
    if len(blk) < TL:
        continue
    op, = struct.unpack('>H', blk[OP:OP + 2])
    if op in (HEAD, CHUNK, FINAL):
        events.append((t - t0, op, blk))

if not events:
    sys.exit('NO SCENE OPS AT ALL — wrong capture, wrong MAC filter, or a snaplen '
             'below 50 bytes. Prove the reader works on a known-good capture first.')

print(f'{len(events)} scene-op frames'
      + (f' from {sys.argv[2]}' if want else ''))
c = collections.Counter(op for _, op, _ in events)
print(f'  headers {c[HEAD]}   chunks {c[CHUNK]}   finals {c[FINAL]}')

runs, cur = [], None
orphan_chunks = 0
for t, op, blk in events:
    if op == HEAD:
        total, = struct.unpack('>H', blk[TOT:TOT + 2])
        cur = {'t': t, 'total': total, 'body': bytearray(blk[HPAY:HPAY + HEAD_B]),
               'n': 0, 'dup': 0, 'prev': None}
    elif op == CHUNK:
        if cur is None:
            orphan_chunks += 1
            continue
        pay = blk[CPAY:CPAY + CHUNK_B]
        if pay == cur['prev']:
            cur['dup'] += 1
        cur['prev'] = pay
        cur['body'] += pay
        cur['n'] += 1
    elif op == FINAL:
        if cur is None:
            continue
        cur['body'] += blk[CPAY:CPAY + FINAL_B]
        cur['end'] = t
        runs.append(cur)
        cur = None

if orphan_chunks:
    print(f'  !! {orphan_chunks} chunks arrived with NO header open — bytes the box '
          f'can only discard')
if cur is not None:
    print(f'  !! a transfer opened at {cur["t"]:.2f}s and never reached its final '
          f'({cur["n"]} chunks in) — the box is left in reassembly')

print(f'\n{len(runs)} header->final run(s):')
for r in runs:
    ok = len(r['body']) == r['total'] and r['n'] == NCHUNKS
    print(f"  {r['t']:6.2f}s -> {r['end']:6.2f}s  ({r['end']-r['t']:.3f}s)  "
          f"chunks={r['n']:3d}  bytes={len(r['body']):5d}  declared={r['total']}  "
          f"consecutive-dups={r['dup']:3d}  {'COMPLETE' if ok else 'INCOMPLETE'}")

good = [r for r in runs if len(r['body']) == r['total'] and r['n'] == NCHUNKS]
print(f'\n{len(good)} complete transfer(s) of {runs and len(runs) or 0}')
if good:
    b = bytes(good[0]['body'])
    print(f'  first complete body sha256[:12] = '
          f'{__import__("hashlib").sha256(b).hexdigest()[:12]}')
