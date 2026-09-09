#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""Bisect a REAC enrolment with pcaps instead of a daemon.

A tcpreplay of the S-1608's own recorded enrolment onto the S-0808 GETS THE GRANT, and
every build of this daemon is refused, so the difference is in the FRAMES and can be found
by moving the granted file one step at a time toward what we send. Each variant below
changes exactly ONE thing and is byte-exact elsewhere; timestamps are never touched, so the
replay keeps the original 8000 pps cadence.

The pcap format is four fields and a payload, so this reads and writes it directly rather
than depending on a capture library that would have to be installed on the rig.
"""
import os, struct, sys

GRANTED = "s1608-enrol-replay.pcap"      # the file that WAS granted
OURS    = "rig-0.5.6-5-box.pcap"         # a daemon run that was refused
S1608   = bytes.fromhex("0040abc48041")  # the granted box's source
OURMAC  = bytes.fromhex("0040ab9b282d")  # our Roland-OUI stand-in
BOX     = bytes.fromhex("0040abc4dc9c")  # the S-0808, in master mode


def read(path, only_src=None):
    """[(ts_sec, ts_usec, frame_bytes)], in file order."""
    out = []
    with open(path, "rb") as f:
        gh = f.read(24)
        if struct.unpack("<I", gh[:4])[0] != 0xA1B2C3D4:
            sys.exit("%s: not a little-endian microsecond pcap" % path)
        while True:
            h = f.read(16)
            if len(h) < 16:
                break
            ts, tu, il, _ol = struct.unpack("<IIII", h)
            d = f.read(il)
            if only_src is None or d[6:12] == only_src:
                out.append([ts, tu, bytearray(d)])
    return out


def write(path, recs):
    with open(path, "wb") as f:
        f.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 262144, 1))
        for ts, tu, d in recs:
            f.write(struct.pack("<IIII", ts, tu, len(d), len(d)))
            f.write(bytes(d))


def is_bcast(d):     return d[0:6] == b"\xff" * 6
def counter(d):      return d[14] | (d[15] << 8)
def set_counter(d, c):
    d[14] = c & 0xFF
    d[15] = (c >> 8) & 0xFF
def ctrl_zero(d):    return all(b == 0 for b in d[18:50])
def is_ctrl(d):      return d[16] == 0xCD and d[17] == 0xEA   # a cdea control frame


def main(tmp):
    src = os.path.join(tmp, GRANTED)
    dst = os.path.join(tmp, "variants")
    os.makedirs(dst, exist_ok=True)
    base = read(src)
    ours = read(os.path.join(tmp, OURS), only_src=OURMAC)
    # our unicast frames, taken AFTER our first flood run so they are steady-state ones
    our_uni = [r for r in ours if not is_bcast(r[2])]
    made = []

    def emit(name, recs, what):
        write(os.path.join(dst, name), recs)
        made.append((name, what, len(recs)))

    def clone():
        return [[ts, tu, bytearray(d)] for ts, tu, d in base]

    # V1 — the source address, and nothing else.
    v = clone()
    for _, _, d in v:
        d[6:12] = OURMAC
    emit("V1-src-mac.pcap", v, "source MAC -> our Roland-OUI stand-in 00:40:ab:9b:28:2d")

    # V2 — the slots, silent. Everything up to the two-byte end marker.
    v = clone()
    for _, _, d in v:
        for i in range(52, len(d) - 2):
            d[i] = 0
    emit("V2-silent-slots.pcap", v, "every audio slot zeroed (we send digital silence)")

    # V3 — the counter rebased to start at zero; V3b frozen.
    v = clone()
    first = counter(v[0][2])
    for _, _, d in v:
        set_counter(d, (counter(d) - first) & 0xFFFF)
    emit("V3-counter-rebased.pcap", v, "frame counter rebased to start at 0")
    v = clone()
    for _, _, d in v:
        set_counter(d, first)
    emit("V3b-counter-frozen.pcap", v, "frame counter frozen at its first value")

    # V4 — the flood as long as ours: 11599 frames, not 5459 (measured on the rig run;
    # our FSM holds the flood open until a master-kind frame arrives, and this box sends
    # one about once a second).
    flood = [r for r in base if is_bcast(r[2])]
    rest = [r for r in base if not is_bcast(r[2])]
    v = []
    step_us = 125            # 8000 pps, the cadence the rest of the file keeps
    ts, tu = flood[0][0], flood[0][1]
    for i in range(11599):
        f = flood[i % len(flood)]
        d = bytearray(f[2])
        set_counter(d, (counter(flood[0][2]) + i) & 0xFFFF)
        v.append([ts, tu, d])
        tu += step_us
        if tu >= 1000000:
            tu -= 1000000
            ts += 1
    shift_s, shift_us = ts - rest[0][0], tu - rest[0][1]
    for r in rest:
        nu = r[1] + shift_us
        ns = r[0] + shift_s + (1 if nu >= 1000000 else 0)
        v.append([ns, nu % 1000000, bytearray(r[2])])
    emit("V4-long-flood.pcap", v, "flood stretched from 5459 to 11599 frames, as ours runs")

    # V5 — the NEGATIVE CONTROL. The pre-grant unicast claims ESTABLISHED (00 7a x 16),
    # which is the bug 499aef1 fixed. This one MUST be refused, or the scanner is not
    # measuring what we think it is.
    v = clone()
    for _, _, d in v:
        if not is_bcast(d) and ctrl_zero(d):
            for i in range(16):
                d[18 + i * 2] = 0x00
                d[18 + i * 2 + 1] = 0x7A
    emit("V5-pregrant-descriptor.pcap", v,
         "NEGATIVE CONTROL: pre-grant unicast claims ESTABLISHED (007a) — must NOT be granted")

    # V6 — their file, our announce and burst carriers.
    ourctl = [r for r in our_uni if is_ctrl(r[2])][:4]
    v = clone()
    if ourctl:
        k = 0
        for r in v:
            if not is_bcast(r[2]) and is_ctrl(r[2]):
                r[2] = bytearray(ourctl[k % len(ourctl)][2])
                k += 1
    emit("V6-our-control-frames.pcap", v,
         "announce and cold-connect frames replaced by ours, the rest theirs (%d taken)" % len(ourctl))

    # V7 — their file, our first 30 unicast frames.
    v = clone()
    k = 0
    for i, r in enumerate(v):
        if not is_bcast(r[2]) and k < 30 and k < len(our_uni):
            r[2] = bytearray(our_uni[k][2])
            k += 1
    emit("V7-our-first-30-unicast.pcap", v, "our first 30 unicast frames, the rest theirs")

    # V8 — THE REVERSE. Our own run, with only the source address made theirs.
    v = [[ts, tu, bytearray(d)] for ts, tu, d in ours]
    for _, _, d in v:
        d[6:12] = S1608
    emit("V8-ours-with-their-mac.pcap", v,
         "OUR capture with ONLY the source MAC rewritten to the S-1608's")

    # V0 — THE CONTROL. Our own refused run, replayed unmodified. The rig has already
    # scored the live daemon at zero echoes; if its recorded frames also score zero the
    # method is validated end to end, and V8 (this same file with only the source MAC
    # changed) isolates the MAC in a single step.
    emit("V0-ours-unmodified.pcap", [[ts, tu, bytearray(d)] for ts, tu, d in ours],
         "CONTROL: our own refused frames, unmodified — expected 0 echoes")

    order = """# REAC enrolment bisect — replay these one at a time (30 s each)

A tcpreplay of `s1608-enrol-replay.pcap` GETS THE GRANT from the S-0808; every build of the
daemon is refused. Each file below is that granted capture with EXACTLY ONE thing moved
toward what the daemon sends, byte-exact elsewhere and with the original timestamps, so the
replay keeps its 8000 pps cadence. `V8` is the reverse: our own refused run with one thing
made theirs.

Score each with `grant-scan.py` on the result capture: 3 echoes = granted, 0 = refused.
**Replay in this order** — it is my likelihood order, and the first refusal is the answer.

| order | file | what changed | why it is here |
|---|---|---|---|
| 1 | `V2-silent-slots.pcap` | every audio slot zeroed | the only difference visible in EVERY frame: their flood and pre-grant unicast carry live samples, ours carry digital silence because nothing is patched to the sink |
| 2 | `V1-src-mac.pcap` | source MAC -> ours | the one field a box could plausibly key an enrolment on that we cannot otherwise test; the rig already tried our daemon FROM their address and was refused, which argues against it, but not with the rest of the frame identical |
| 3 | `V4-long-flood.pcap` | flood 5459 -> 11599 frames | measured: our flood runs 11599 frames / 1.65 s because the FSM holds it open until a master-kind frame arrives, and this box sends one about once a second |
| 4 | `V7-our-first-30-unicast.pcap` | our first 30 unicast frames | if 1-3 all pass, the difference is inside the frames themselves and this narrows it to the opening of the unicast phase |
| 5 | `V6-our-control-frames.pcap` | our announce + burst carriers | the control frames were byte-compared and matched, so this should PASS; if it fails, the comparison missed something outside the block |
| 6 | `V3-counter-rebased.pcap` | counter starts at 0 | the replay already granted with a counter unrelated to the box's, so this should PASS |
| 7 | `V3b-counter-frozen.pcap` | counter frozen | a stronger form of the same question |
| 8 | `V5-pregrant-descriptor.pcap` | **NEGATIVE CONTROL** | claims ESTABLISHED before the grant. This one MUST be REFUSED. If it is granted, the scanner is not measuring the thing we think it is and every result above is void |
| 9 | `V8-ours-with-their-mac.pcap` | OUR run, THEIR MAC | if this is granted the MAC is the whole story and everything else we send already matches |

Run V5 EARLY if anything surprises you: it is the only file here whose expected answer is
"refused", and it is what makes a pass anywhere else mean something.
"""
    with open(os.path.join(dst, "README.md"), "w") as f:
        f.write(order)
    for name, what, n in made:
        print("%-32s %7d frames  %s" % (name, n, what))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
