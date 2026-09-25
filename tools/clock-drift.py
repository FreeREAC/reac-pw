#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""Where the drift is, in three passive measurements that need no rebuild.

The question this answers is the one that decides the fix: a 900 ppm error is
2700 times a crystal, so it is STRUCTURAL, and a rate matcher laid over a
structural error makes it inaudible rather than absent. Each mode below isolates
one link in the chain, and every one of them is read-only — nothing is written to
the daemon, the graph or the wire.

  ref     the hardware clock the graph is disciplined to, against CLOCK_MONOTONIC.
          Read from /proc/asound/<card>/pcm<N>p/sub0/{hw_ptr,tstamp}: the sound
          card's own frame counter and the system time it was stamped at. If this
          is not within a few tens of ppm, the oscillator IS the story and nothing
          else here matters.

  wire    what actually left the NIC, against CLOCK_MONOTONIC. Read from
          /sys/class/net/<if>/statistics/{tx,rx}_packets, which counts frames the
          driver accepted, so it cannot be fooled by capture-side loss. TX is our
          pacer; RX is the box's return, i.e. the box's own crystal.

  gaps    WHY the wire rate is short: slots the pacer never ran, or frames it
          built and failed to send. These look identical in time and are told
          apart by the REAC sequence counter at offset 14 — the pacer advances it
          once per loop iteration, so a slot that never ran leaves the counter
          CONTIGUOUS across the hole and a frame lost to sendto() leaves a GAP.
          Needs a capture (see --pcap); snaplen 20 is enough and keeps 90 s under
          13 MB.

Every mode prints the size of what it measured before the result it derives, so
an empty scan can never be mistaken for a clean one.
"""
import argparse, struct, sys, time
import os as _os
sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from facts import FACTS   # the protocol's numbers, from their one declaration (tools/facts.py)


def ref(card, dev, dur):
    p = f"/proc/asound/{card}/pcm{dev}p/sub0/status"
    hw = f"/proc/asound/{card}/pcm{dev}p/sub0/hw_params"
    rate = None
    for line in open(hw):
        if line.startswith("rate:"):
            rate = float(line.split()[1])
    if rate is None:
        sys.exit(f"{hw}: no rate — is the device open?")

    def rd():
        d = {}
        for line in open(p):
            if ":" in line:
                k, v = line.split(":", 1)
                d[k.strip()] = v.strip()
        if d.get("state") != "RUNNING":
            sys.exit(f"{p}: state is {d.get('state')!r}, not RUNNING — nothing to measure")
        return float(d["tstamp"]), int(d["hw_ptr"])

    t0, h0 = rd()
    time.sleep(dur)
    t1, h1 = rd()
    dt, dh = t1 - t0, h1 - h0
    if dh <= 0:
        sys.exit("hw_ptr did not advance — the probe cannot detect presence, so its silence proves nothing")
    obs = dh / dt
    print(f"reference: {card}/pcm{dev}p  nominal {rate:.0f} Hz")
    print(f"  window {dt:.3f} s, {dh} frames advanced")
    print(f"  observed {obs:.4f} Hz  ->  {(obs / rate - 1) * 1e6:+.1f} ppm vs CLOCK_MONOTONIC")


def wire(ifaces, dur, fps):
    def snap():
        t = time.clock_gettime(time.CLOCK_MONOTONIC)
        out = {}
        for i in ifaces:
            b = f"/sys/class/net/{i}/statistics/"
            out[i] = (int(open(b + "tx_packets").read()), int(open(b + "rx_packets").read()))
        return (t + time.clock_gettime(time.CLOCK_MONOTONIC)) / 2, out

    t0, a = snap()
    time.sleep(dur)
    t1, b = snap()
    dt = t1 - t0
    print(f"wire: window {dt:.4f} s (CLOCK_MONOTONIC), nominal {fps} fps")
    for i in ifaces:
        for name, idx in (("TX (our pacer)", 0), ("RX (the box)  ", 1)):
            d = b[i][idx] - a[i][idx]
            if d == 0:
                print(f"  {i} {name}: NO PACKETS — the probe saw nothing, which is not the same as nothing happening")
                continue
            pps = d / dt
            print(f"  {i} {name}: {d} pkts  {pps:.4f} pps  {(pps / fps - 1) * 1e6:+.1f} ppm")


def gaps(path, fps):
    period = 1.0 / fps
    f = open(path, "rb")
    gh = f.read(24)
    magic, = struct.unpack("<I", gh[:4])
    if magic not in (0xA1B2C3D4, 0xA1B23C4D):
        sys.exit(f"{path}: not a little-endian pcap ({magic:#x})")
    den = 1e9 if magic == 0xA1B23C4D else 1e6
    recs = []
    while True:
        h = f.read(16)
        if len(h) < 16:
            break
        s, u, cl, _ = struct.unpack("<IIII", h)
        d = f.read(cl)
        if len(d) < cl:
            break
        if cl >= 16:
            recs.append((s + u / den, struct.unpack("<H", d[14:16])[0]))
    if len(recs) < 2:
        sys.exit(f"{path}: {len(recs)} frames — nothing to measure. Capture with "
                 f"`tcpdump -i IF -s 20 --time-stamp-precision=nano -w FILE "
                 f"'ether proto 0x8819 and ether src <our mac>'`")
    dur = recs[-1][0] - recs[0][0]
    print(f"gaps: {len(recs)} frames over {dur:.3f} s  (the probe sees traffic)")
    print(f"  emitted {(len(recs) - 1) / dur:.4f} pps  "
          f"{(((len(recs) - 1) / dur) / fps - 1) * 1e6:+.1f} ppm vs {fps}")

    counter_gaps = 0
    lost_to_send = 0
    holes = 0
    hole_excess = 0.0
    for i in range(1, len(recs)):
        dt = recs[i][0] - recs[i - 1][0]
        dc = (recs[i][1] - recs[i - 1][1]) & 0xFFFF
        if dc != 1:
            counter_gaps += 1
            lost_to_send += dc - 1
        if dt > 1.5 * period:
            holes += 1
            hole_excess += dt - period
    print(f"  time holes > 1.5 slots: {holes} ({holes / dur:.2f}/s), "
          f"{hole_excess * 1e3:.1f} ms of wire time inside them")
    print(f"  sequence-counter gaps:  {counter_gaps}  "
          f"({lost_to_send} frames built and never sent)")
    print()
    if counter_gaps == 0 and holes:
        print("  VERDICT: every hole is a slot the loop NEVER RAN. The counter is")
        print("  contiguous across all of them, so no frame was built and lost —")
        print("  sendto() did not fail once. The deficit is overslept slots being")
        print("  abandoned, which is a pacer question and not a socket one.")
    elif lost_to_send:
        print("  VERDICT: frames were built, counter-stamped and never reached the")
        print("  wire. That is sendto() failing during established audio, and it")
        print("  outranks every other finding here.")
    else:
        print("  VERDICT: no holes and no counter gaps — the cadence is clean.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)
    r = sub.add_parser("ref"); r.add_argument("--card", default="card0")
    r.add_argument("--dev", default="0"); r.add_argument("--seconds", type=float, default=30)
    w = sub.add_parser("wire"); w.add_argument("iface", nargs="+")
    w.add_argument("--seconds", type=float, default=180); w.add_argument("--fps", type=float, default=FACTS["PKT_RATE_48K"])
    g = sub.add_parser("gaps"); g.add_argument("pcap"); g.add_argument("--fps", type=float, default=FACTS["PKT_RATE_48K"])
    a = ap.parse_args()
    if a.mode == "ref":
        ref(a.card, a.dev, a.seconds)
    elif a.mode == "wire":
        wire(a.iface, a.seconds, a.fps)
    else:
        gaps(a.pcap, a.fps)


if __name__ == "__main__":
    main()
