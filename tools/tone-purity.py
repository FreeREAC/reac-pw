#!/usr/bin/env python3
"""How much of a channel's energy sits at one frequency?

An ELECTRICAL loopback of a synthesised tone is nearly pure: almost all the
energy lands in the target bin. A microphone hearing that tone through a speaker
carries the room with it — reflections, noise, and whatever else is audible — so
its purity is far lower and it drifts. That difference is what tells an
electrical source from an acoustic one, and it decides whether a gain curve can
be measured at all.

  tools/tone-purity.py capture.wav 1000 [channel]
"""
import struct, sys, math, cmath

def read(path):
    d = open(path, 'rb').read()
    i, fmt, data = 12, None, None
    while i + 8 <= len(d):
        cid, sz = d[i:i+4], struct.unpack('<I', d[i+4:i+8])[0]
        if cid == b'fmt ':  fmt = struct.unpack('<HHIIHH', d[i+8:i+24])
        elif cid == b'data': data = d[i+8:i+8+sz]
        i += 8 + sz + (sz & 1)
    return fmt, data

fmt, data = read(sys.argv[1])
freq = float(sys.argv[2])
ch_want = int(sys.argv[3]) - 1 if len(sys.argv) > 3 else None
ch, rate = fmt[1], fmt[2]
n = len(data) // 4 // ch
s = struct.unpack('<%df' % (n * ch), data[:n * ch * 4])

print(f'{"ch":>3} {"rms dBFS":>9} {"tone dBFS":>10} {"purity":>8} {"peak":>7} {"h2+h3":>7}')
for c in range(ch):
    if ch_want is not None and c != ch_want:
        continue
    v = s[c::ch]
    N = min(len(v), rate)                      # 1 s window, integer bins
    k = round(freq * N / rate)
    acc = 0j
    for i in range(N):
        acc += v[i] * cmath.exp(-2j * math.pi * k * i / N)
    tone = 2.0 * abs(acc) / N                  # amplitude at the bin
    rms = math.sqrt(sum(x * x for x in v[:N]) / N)
    trms = tone / math.sqrt(2.0)
    db = lambda x: -999.0 if x <= 0 else 20 * math.log10(x)
    pur = (trms / rms) if rms > 0 else 0.0
    pk = max(abs(x) for x in v[:N]) if N else 0.0
    # Harmonics: a clipped sine grows them fast, and a clipped reading fakes a
    # compressed curve — which is the exact shape of the non-linearity under test.
    h = 0.0
    for hn in (2, 3):
        kk = round(freq * hn * N / rate)
        a = 0j
        for i in range(N):
            a += v[i] * cmath.exp(-2j * math.pi * kk * i / N)
        h += (2.0 * abs(a) / N) ** 2
    h = math.sqrt(h) / math.sqrt(2.0)
    flag = '  CLIPPING' if pk >= 0.99 else ''
    print(f'{c+1:>3} {db(rms):>9.1f} {db(trms):>10.1f} {pur*100:>7.1f}% '
          f'{db(pk):>7.1f} {db(h) - db(trms):>7.1f}{flag}')
