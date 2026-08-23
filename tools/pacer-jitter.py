#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# Inter-frame timing of a REAC segment, from a pcap, split by source MAC.
#
# This is the instrument the fast-path work is graded against: reac-pw's pacer must
# stay inside the envelope of the real hardware sharing the wire with it, so the
# capture is taken with BOTH directions in it and the box's own return is reported
# beside ours as the control. A run that shows only our own source proves nothing —
# it cannot tell a tight pacer from a quiet wire.
#
# The TRIMMED stddev (0.1% off each tail) is reported next to the raw one because a
# single capture-side gap is not pacer jitter, and one such outlier moves the raw
# figure by more than any change this work makes.
#
#   sudo tcpdump -i IF -s 64 -j adapter_unsynced --time-stamp-precision=nano \
#        -w cap.pcap 'ether proto 0x8819'
#   tools/pacer-jitter.py cap.pcap
import struct, sys, statistics as st
from collections import defaultdict


def read(path):
    f = open(path, 'rb')
    magic, = struct.unpack('<I', f.read(24)[:4])
    div = {0xa1b2c3d4: 1e6, 0xa1b23c4d: 1e9}.get(magic)
    if div is None:
        raise SystemExit('%s: not a little-endian pcap (magic %08x)' % (path, magic))
    out = []
    while True:
        r = f.read(16)
        if len(r) < 16:
            break
        ts, tu, incl, orig = struct.unpack('<IIII', r)
        d = f.read(incl)
        if len(d) < incl:
            break
        out.append((ts + tu / div, d, orig))
    return out


def main(path):
    pk = read(path)
    if not pk:
        raise SystemExit('%s: empty capture — a silent probe and a silent wire '
                         'look identical, so this is a failure, not a result' % path)
    by = defaultdict(list)
    for t, d, orig in pk:
        if len(d) >= 14:
            by[(':'.join('%02x' % b for b in d[6:12]), orig)].append(t)
    print('# %s: %d frames over %.3f s' % (path, len(pk), pk[-1][0] - pk[0][0]))
    for (src, ln), ts in sorted(by.items(), key=lambda kv: -len(kv[1])):
        if len(ts) < 50:
            continue
        d = sorted((ts[i + 1] - ts[i]) * 1e6 for i in range(len(ts) - 1))
        n = len(d)
        mean = st.fmean(d)
        trimmed = d[int(n * 0.001):int(n * 0.999)]
        print('src=%s len=%-5d n=%-6d pps=%7.1f mean=%7.2fus p50=%7.2f '
              'sd=%6.2f trimmed_sd=%5.2f min=%7.2f p99=%7.2f max=%8.2f'
              % (src, ln, n, 1e6 / mean, mean, d[n // 2], st.pstdev(d),
                 st.pstdev(trimmed), d[0], d[int(n * 0.99)], d[-1]))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: pacer-jitter.py CAPTURE.pcap')
    main(sys.argv[1])
