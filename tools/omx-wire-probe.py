#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""omx-wire-probe: one-way latency and drops of REAC frames over a real cable, both ends on one host.

Usage (root):  omx-wire-probe.py NETNS_A IF_A NETNS_B IF_B SECONDS

The two ends of the cable are two NICs of ONE box, each in its own network namespace. An AF_PACKET socket stays bound
to the namespace it was opened in, so this one process opens a socket on IF_A inside NETNS_A and one on IF_B inside
NETNS_B. Both see every frame their NIC sends AND receives, stamped by the kernel (SO_TIMESTAMPNS) on ONE clock. A REAC
frame (EtherType 0x8819) carries a 16-bit LE counter at [14:16] that steps by one per frame per talker, so a frame seen
leaving one end is matched by (source MAC, counter) with the same frame arriving at the other:

  latency  = rx stamp at the far end - tx stamp at the near end (NIC queue + wire + NIC + interrupt), per direction;
  dropped  = a frame that left one end and never arrived at the other;
  gaps     = counter steps that were not +1 at the RECEIVING end (seq_gaps' measure, libreac tools/seq_gaps.c).

A frame the probe itself lost (the kernel's PACKET_STATISTICS tp_drops on either socket) is NOT a wire drop: it is
reported on its own line and makes the run NOT MEASURED, never green. An absence claim needs a capture that held.

Output: one 'dir' line per direction, a 'capture' line, then 'OMX-WIRE ok|FAILED|NOT-MEASURED'. Exit 0 / 1 / 2.
"""
import ctypes
import errno
import os
import select
import socket
import struct
import sys
import time

ETH_P_ALL = 0x0003
REAC = b"\x88\x19"
SOL_PACKET = 263
PACKET_STATISTICS = 6
SO_TIMESTAMPNS = 35
SCM_TIMESTAMPNS = SO_TIMESTAMPNS
PACKET_OUTGOING = 4
CLONE_NEWNET = 0x40000000
# a frame not seen at the far end within this long is dropped (a REAC frame crosses a cable in microseconds)
MATCH_WINDOW_NS = 200_000_000

libc = ctypes.CDLL(None, use_errno=True)


def open_in(netns: str, ifname: str) -> socket.socket:
    """An AF_PACKET socket on ifname, opened inside /run/netns/<netns>; it stays there after we leave."""
    fd = os.open(f"/run/netns/{netns}", os.O_RDONLY)
    try:
        if libc.setns(fd, CLONE_NEWNET) != 0:
            raise OSError(ctypes.get_errno(), f"setns {netns}")
        s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
        s.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPNS, 1)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 32 << 20)
        s.bind((ifname, ETH_P_ALL))
        s.setblocking(False)
        return s
    finally:
        os.close(fd)


def tp_drops(s: socket.socket) -> tuple[int, int]:
    """(packets, drops) since the last read; reading resets the kernel's counters."""
    raw = s.getsockopt(SOL_PACKET, PACKET_STATISTICS, 8)
    return struct.unpack("II", raw)


class Direction:
    """Frames one talker sends from its end, met at the other."""

    def __init__(self, name: str):
        self.name = name
        self.pending: dict[int, int] = {}   # counter -> tx stamp, frames sent and not yet arrived
        self.early: dict[int, int] = {}     # counter -> rx stamp, arrivals read before their departure was
        self.sent = self.arrived = self.dropped = self.gaps = self.missing = 0
        self.lat: list[int] = []
        self.last_rx: int | None = None
        self.unmatched_rx = 0
        self.tx_sock: socket.socket | None = None

    def tx(self, counter: int, ns: int) -> None:
        self.sent += 1
        r = self.early.pop(counter, None)
        if r is not None:            # the two sockets are read in turn: the arrival may be read first
            self.met(r - ns)
            return
        if counter in self.pending:  # the counter wrapped with that frame still unmet: it never arrived
            self.dropped += 1
            print(f"lost {self.name} counter={counter} (sent again before the first arrived) "
                  f"last_arrived_counter={self.last_rx}", file=sys.stderr)
        self.pending[counter] = ns

    def met(self, lat_ns: int) -> None:
        self.arrived += 1
        self.lat.append(lat_ns)

    def rx(self, counter: int, ns: int) -> None:
        if self.last_rx is not None:
            step = (counter - self.last_rx) & 0xFFFF
            if step != 1:
                self.gaps += 1
                self.missing += (step - 1) & 0xFFFF
        self.last_rx = counter
        t = self.pending.pop(counter, None)
        if t is None:
            self.early[counter] = ns  # its departure not read yet (or sent before the probe started)
            return
        self.met(ns - t)

    def expire(self, now_ns: int) -> None:
        """now_ns is the newest stamp READ on the receiving socket, never the wall clock: a reader that fell behind
        still holds the arrivals it has not read, and they are not losses."""
        stale = [c for c, t in self.pending.items() if now_ns - t > MATCH_WINDOW_NS]
        for c in stale:
            t = self.pending.pop(c)
            self.dropped += 1
            if self.dropped <= 20:   # name each loss: what left, when, and what the far end had seen by then
                print(f"lost {self.name} counter={c} left_at={t} far_end_newest={now_ns} last_arrived_counter={self.last_rx}",
                      file=sys.stderr)
        # an arrival whose departure never showed: the probe's tx side missed it (counted, never a wire drop)
        for c in [c for c, t in self.early.items() if now_ns - t > MATCH_WINDOW_NS]:
            del self.early[c]
            self.unmatched_rx += 1

    def line(self) -> str:
        lat = sorted(self.lat)

        def pct(p: float) -> str:
            return f"{lat[min(len(lat) - 1, int(p * len(lat)))] / 1000:.1f}" if lat else "-"

        return (f"dir {self.name} sent={self.sent} arrived={self.arrived} dropped={self.dropped} gaps={self.gaps} "
                f"missing={self.missing} unmatched_rx={self.unmatched_rx} latency_us p50={pct(0.50)} p99={pct(0.99)} "
                f"p999={pct(0.999)} max={(lat[-1] / 1000 if lat else 0):.1f}")


def main() -> int:
    if len(sys.argv) != 6:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    ns_a, if_a, ns_b, if_b, secs = sys.argv[1:6]
    secs_f = float(secs)
    sa, sb = open_in(ns_a, if_a), open_in(ns_b, if_b)
    # the window starts with BOTH ends open: what queued on the first socket before the second existed is discarded (at
    # 8000 frames/s the few ms between the two opens are ~65 departures whose arrivals were never readable)
    for s in (sa, sb):
        while True:
            try:
                s.recv(2048)
            except BlockingIOError:
                break
    tp_drops(sa), tp_drops(sb)   # zero the counters: only the window counts
    # ONE start instant for both ends, taken after both drains: a frame stamped before it is outside the window at
    # either end. (Draining the two in turn leaves a gap: a frame leaving after the first drain and arriving before the
    # second ends would keep its departure and lose its arrival.)
    t0 = time.clock_gettime_ns(time.CLOCK_REALTIME)
    # talker -> Direction, decided by the end a frame is first seen LEAVING (PACKET_OUTGOING)
    by_src: dict[bytes, Direction] = {}
    names = {sa: if_a, sb: if_b}
    bufsize, ancsize = 2048, socket.CMSG_SPACE(16)
    end = time.monotonic() + secs_f
    last_expire = 0.0
    newest = {sa: 0, sb: 0}       # the newest kernel stamp read on each socket
    down = {sa: 0, sb: 0}         # reads refused because the interface was down (ENETDOWN)
    while True:
        now = time.monotonic()
        if now >= end:
            break
        r, _, _ = select.select([sa, sb], [], [], 0.2)
        for s in r:
            while True:
                try:
                    data, anc, _flags, addr = s.recvmsg(bufsize, ancsize)
                except BlockingIOError:
                    break
                except OSError as e:   # the link went down under us: an outage to count, not a crash
                    if e.errno != errno.ENETDOWN:
                        raise
                    down[s] += 1
                    break
                if len(data) < 16 or data[12:14] != REAC:
                    continue
                ns = next((struct.unpack("qq", d[:16]) for lvl, typ, d in anc
                           if lvl == socket.SOL_SOCKET and typ == SCM_TIMESTAMPNS), None)
                if ns is None:
                    continue
                stamp = ns[0] * 1_000_000_000 + ns[1]
                if stamp < t0:
                    continue
                newest[s] = max(newest[s], stamp)
                src, counter = data[6:12], data[14] | (data[15] << 8)
                out = addr[2] == PACKET_OUTGOING
                d = by_src.get(src)
                if d is None:
                    # a talker is known from WHICHEVER end reads it first: its first arrival may be read before its
                    # first departure (the two sockets are read in turn), and an arrival on one end means it sends
                    # from the other. Created only on a departure, that first frame was lost to the probe, never the wire.
                    other = sb if s is sa else sa
                    tx_sock = s if out else other
                    d = by_src[src] = Direction(f"{names[tx_sock]}->{names[other if out else s]}")
                    d.tx_sock = tx_sock
                if out and d.tx_sock is s:
                    d.tx(counter, stamp)
                elif not out and d.tx_sock is not s:
                    d.rx(counter, stamp)
        if now - last_expire > 0.05:
            for d in by_src.values():
                d.expire(newest[sb if d.tx_sock is sa else sa])
            last_expire = now
    # the frames still in flight at the end are neither met nor lost
    time.sleep(0.25)
    pa, da = tp_drops(sa)
    pb, db = tp_drops(sb)
    for d in by_src.values():
        print(d.line())
    print(f"capture {if_a} packets={pa} probe_drops={da} enetdown={down[sa]} {if_b} packets={pb} probe_drops={db} "
          f"enetdown={down[sb]} seconds={secs_f:g}")
    if da or db or not by_src:
        why = "the probe itself lost frames" if (da or db) else "no REAC talker seen on either end"
        print(f"OMX-WIRE NOT-MEASURED: {why}")
        return 2
    # a frame lost on the wire is both unmet (dropped) and a hole at the far end (gaps): either one is red, never summed
    lost = sum(d.dropped for d in by_src.values())
    holes = sum(d.gaps for d in by_src.values())
    silent = [d.name for d in by_src.values() if d.arrived == 0]
    if lost or holes or silent:
        print(f"OMX-WIRE FAILED: {lost} frames dropped, {holes} counter gaps at the receiving ends"
              + (f"; nothing arrived on {', '.join(silent)}" if silent else ""))
        return 1
    print(f"OMX-WIRE ok: {len(by_src)} directions, 0 frames lost in {secs_f:g} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
