# reac-pw as a MIXER (master) — establishment + lock protocol (RE, 2026-07-12)

Goal: reac-pw impersonates a real Roland desk (M-200 first, then M-300/M-5000) so
a real stagebox slaves to **it**. The establishment automaton is the SAME state
diagram as the box side (see `REAC-BOX-STATE-DIAGRAM.md`) — the box's frames are
the transition events; here they drive the MASTER's state. Roles inverted, the
protocol is identical.

All findings below are transcribed byte-exact from
`matrix-m200-s0808-2026-07-11.pcap` (a real M-200 granting a real S-0808, box
**locked solid**) and re-verified live on the rig (reac-pw master on `enp131s0`,
a real S-0808 on the same switch).

## Status (2026-07-12)

Live against the real S-0808:

```
recognized box = S-0808 (8 in / 8 out)      ← recognizer #137, autodetect
PROBING -> GRANTING (rx CONFIG)             ← warm-relink fires the grant burst
GRANTING -> ESTABLISHED (rx UNICAST)        ← after the FULL 96 ms / 32-frame burst
```

reac-pw's downstream now **byte-matches a real M-200** in both phases:
- establishment: the 32-frame grant burst, byte-exact;
- locked: cfea 1/s + chanmap 1/s + filler, and nothing else — verified on the
  wire (`sendto` classification: `cfea 1.00/s, cdea 01030019 1.00/s`, 0 probe/sub).

**Box status light:** with only the grant burst in place the light still BLINKED
(observed on the rig). The blink was NOT the grant (a full 96 ms byte-exact burst
was measured on the wire, `tx_packets` confirming ~4000 fps downstream) — it was
the **locked keep-alive rate**: reac-pw was sending the chanmap at ~0.5/s vs the
real desk's 1.00/s. The 1/s locked-cadence fix (below) landed AFTER that
observation and is **pending a re-confirmation of the light**.

## The three RE findings (all committed, FreeREAC/reac-pw, design/slave-emulation-scope)

### 1. The grant burst — the master's own 32-frame sweep
A real M-200, on the box's cold-connect / config-announce, emits a **one-shot
burst of 32 distinct `cdea 04 03` frames over ~72 ms** (a leading `04030014`
pair bracketing `04030013` slot-grants that iterate the box's in/out pairs; tail
byte [23] is an additive check `0x80 - Σpayload`), then goes calm. Measured: all
62 grant frames fall in a single 1.5 s cluster, then 40 s of silence.

Two earlier models were **falsified**:
- NOT an echo of the box's JOIN block — the grant is the master's OWN sweep.
- NOT a periodic 1/s stream — the "1 Hz" memory was the control-frame heartbeat
  (every cdea/cfea frame also carries 12 audio samples), documented in the
  re-pacer's "1 Hz click" bug, not the grant.

reac-pw replays the 32 blocks byte-exact (each already sums to 0 over [18:50], so
the checksum re-stamp is a no-op). Selectable per autodetected model.

### 2. Burst-completion accept gate
A warm-relink box unicasts from the first slot, so accepting its unicast
immediately cut the burst to ~1 frame (rig: `GRANTING -> ESTABLISHED` in 0.25 ms).
Fix: gate `GRANTING -> ESTABLISHED` on the FULL 32-frame burst having been
delivered; before that the box unicast only confirms presence and keeps granting.
Anti-#130 holds — establish still needs a box frame (never a blind timer), and the
grant window still expires BACK to PROBING with no accept. Rig: GRANTING now lasts
~96 ms (the full burst) before establishing. Warm-relink CONFIG routes through
GRANTING rather than jumping straight to ESTABLISHED.

### 3. Locked keep-alive cadence — cfea + chanmap, both at 1.00/s
Once established a real M-200 emits ONLY two control frames, each **metronomic at
1.00/s** (measured: 1004 ms gaps, dead steady): cfea and the sub-state-0x03
chanmap. NOTHING else — 0 probes, 0 sub01/sub02. The chanmap is the box's sync
keep-alive (§4: the box's parser recognizes/holds a master on that map). reac-pw's
old cadence ran the chanmap at the hunt rate (1/cycle ≈ 0.37/s), under-sending the
heartbeat. `control_cadence` is now split into the measured HUNT (PROBING) and
LOCK (ESTABLISHED) cadences; locked chanmap runs at 1/s. Verified on the wire.

## Rigorously RULED OUT (do not re-chase)
- **Frame length**: 1492 B is correct (700 = snaplen, 1494 = Ethernet FCS from a
  mirror config — see `SLAVE-EMULATION-SCOPE.md`).
- **Chanmap content**: the 49-window sliding sweep already byte-matches the M-200.
- **"reac-pw isn't transmitting"**: FALSE — `tx_packets` climbs ~4000 fps and the
  `sendto` payloads are well-formed `8819` broadcast frames. A python RX sniffer
  not seeing the host's own outgoing frames is a capture artifact, not reality.

## If the light is STILL blinking after the 1/s cadence fix — next candidate
**TX timing jitter.** The SCHED_FIFO pacer holds p50 = 250 µs (perfect) but shows
occasional gaps to ~500 µs–3.9 ms (σ ≈ 63 µs, ~0.3 % of slots a full slot late;
partly strace perturbation, partly real userspace-RT jitter). A hardware desk
gives the box a rock-solid word-clock; ms-scale gaps can break the box's PLL lock.
This is task #131 (DLL-discipline the pacer to a real clock). Measure the box's
UPSTREAM jitter while locked to reac-pw vs while locked to a real M-200 to confirm
before investing in the clock rework.

## Next, for a COMPLETE mixer product
1. Confirm the light goes solid with the 1/s locked cadence (pending, rig).
2. If not: TX jitter / clock discipline (#131).
3. Real-MAC-as-identity: use the NIC's own MAC (or set the NIC to a Roland MAC) so
   the box's unicast is received natively without promiscuous mode, and to test
   whether the box validates the master OUI.
4. Drive the patches: route the box's upstream audio into PipeWire; feed the
   downstream from the graph (the reac:playback sink is running but idle).
5. Generalise the grant burst + probe specials to M-300 / M-5000 and to the other
   box widths (S-1608 / S-4000S) from their matrix-*.pcap.
