#!/usr/bin/env python3
"""Level of a steady sine on one channel: RMS, peak, and the crest factor.

RMS rather than an FFT bin. A single-bin estimate under-reads badly here because
the REAC path repaces through PipeWire, so the received tone sits slightly off
the generated frequency and a rectangular window leaks it across bins — it read
6.6 dB low on a signal that was demonstrably clean. RMS has no such failure mode
once the tone dominates, and the generator-off control establishes that it does.

CREST (peak - rms) is the validity check, and it is the clipping detector this
measurement needs: a pure sine is 3.01 dB, and a flat-topped one is LESS, because
clipping raises rms toward peak. A clipped reading fakes a compressed curve,
which is the exact shape of the non-linearity under test.

  tools/sine-level.py capture.wav [channel]
"""
import struct, sys, math

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
want = int(sys.argv[2]) - 1 if len(sys.argv) > 2 else None
ch = fmt[1]
n = len(data) // 4 // ch
s = struct.unpack('<%df' % (n * ch), data[:n * ch * 4])
db = lambda x: -999.0 if x <= 0 else 20 * math.log10(x)

for c in range(ch):
    if want is not None and c != want:
        continue
    v = s[c::ch]
    rms = math.sqrt(sum(x * x for x in v) / len(v))
    pk = max(abs(x) for x in v)
    crest = db(pk) - db(rms)
    flag = ''
    if pk >= 0.999:            flag = '  CLIPPING (peak at full scale)'
    elif crest < 2.8:          flag = '  SUSPECT (crest below a sine)'
    elif crest > 3.3:          flag = '  SUSPECT (not a clean sine)'
    print(f'ch {c+1:2d}  rms {db(rms):8.2f} dBFS   peak {db(pk):7.2f}   '
          f'crest {crest:5.2f}{flag}')
