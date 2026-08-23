#!/usr/bin/env python3
"""Per-channel RMS/peak of a float32 WAV, in dBFS.

A REAC node always carries a ~-106.6 dBFS floor, so an EXACT digital zero on
every channel means the capture failed, not that the desk is silent. That case
is called out rather than printed as a row of quiet numbers.
"""
import struct, sys, math

def read(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'RIFF' and d[8:12] == b'WAVE', 'not a WAV'
    i, fmt, data = 12, None, None
    while i + 8 <= len(d):
        cid, sz = d[i:i+4], struct.unpack('<I', d[i+4:i+8])[0]
        body = d[i+8:i+8+sz]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', body[:16])
        elif cid == b'data':
            data = body
        i += 8 + sz + (sz & 1)
    return fmt, data

fmt, data = read(sys.argv[1])
ch, rate, bits = fmt[1], fmt[2], fmt[5]
assert bits == 32, f'expected float32, got {bits}'
n = len(data) // 4 // ch
s = struct.unpack('<%df' % (n * ch), data[:n * ch * 4])

print(f'{n} frames, {ch} ch, {rate} Hz  ({n/rate:.2f} s)')
allzero = True
rows = []
for c in range(ch):
    v = s[c::ch]
    pk = max(abs(x) for x in v) if v else 0.0
    rms = math.sqrt(sum(x * x for x in v) / len(v)) if v else 0.0
    if pk != 0.0:
        allzero = False
    rows.append((c, rms, pk))

if allzero:
    print('FAILED CAPTURE: every channel is an exact digital zero.')
    print('A live REAC node carries a ~-106.6 dBFS floor; this is no signal path,')
    print('not a silent desk. Fix the links before reading anything into it.')
    sys.exit(2)

def db(x):
    return -999.0 if x <= 0 else 20 * math.log10(x)

print(f'{"port":>5} {"rms dBFS":>10} {"peak dBFS":>10}   note')
for c, rms, pk in rows:
    note = ''
    if pk == 0.0:
        note = 'EXACT ZERO — no data on this port'
    elif db(rms) > -60:
        note = '<-- SIGNAL'
    elif db(rms) < -95:
        note = 'floor'
    print(f'{c+1:>5} {db(rms):>10.1f} {db(pk):>10.1f}   {note}')
