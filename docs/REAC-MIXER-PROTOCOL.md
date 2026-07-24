# reac-pw as a MIXER (master) — full establishment protocol (SOLVED, 2026-07-12)

reac-pw impersonates a real Roland desk (M-200 first) so a real stagebox slaves to
**it**. **SOLVED on the rig:** a real S-0808 cold-connects, is granted, reaches its
own ESTABLISHED, lights **SOLID**, and holds a steady 1/s heartbeat with **zero
drops**. All findings transcribed byte-exact from `matrix-m200-s0808-2026-07-11.pcap`
(real M-200 + real S-0808, box solid) and re-verified live.

The establishment automaton is the SAME diagram box-side and mixer-side — roles
inverted, frames identical. Our SLAVE FSM (`reac_fsm.c`, validated task #136) is the
ground-truth stage-box model; the master is its mirror.

## State-by-state — what each side emits, and the transition frames

```
 BOX (stagebox)                         wire                    MIXER (master)
 ─────────────────────────────────────────────────────────────────────────────
 PHY_DOWN                                                       PROBING
   (link up)                                                      emits: PROBE ~500/s burst,
 FLOOD_ANNOUNCE                                                    chanmap, sub01/02, cfea
   emits: broadcast presence-flood  ── FLOOD ──▶  (diagnostic; does NOT grant)
 COLD_CONNECT                                                   PROBING
   emits (unicast, on a retry grid): ── JOIN cdea 04 03 ──▶     rx JOIN ⇒ GRANTING
     0403 0013/0014/0016/001a (JOIN variants)
     0401 001b (NAME = "S-0808")                               GRANTING
     0402 000d (S-0808 extra)                                    emits, in order:
   ◀── ENROLL  cdea 01 03 000d ──                                 1× ENROLL  (arms the box)
   ◀── GRANT   cdea 04 03 burst ──                                32× grant sweep (byte-exact)
                                                                  cfea now carries box-count=1
   rx GRANT ⇒ TX_MUTE                                           GRANTING
     (silent; recovers word-clock                                after full burst ⇒ self-COMMIT
      from master inter-arrival)                                 ESTABLISHED  (do NOT wait for
                                                                  a post-burst unicast; do NOT
   dwell elapses ⇒                                               re-grant the same box's retry
 ESTABLISHED                                                     JOINs — HOLD the stable stream)
   emits (unicast): audio + ── HEARTBEAT cdea 01 03 0001 ──▶    rx HEARTBEAT ⇒ lock confirmed
     01030001 sel 0x81, ~1/s          (definitive)               (reload peer-alive budget)
   LED SOLID                                                    ESTABLISHED
                                                                  emits ONLY: cfea 1/s +
                                                                  chanmap 1/s (the box's sync
                                                                  keep-alive) + filler. 0 probe.
 ─────────────────────────────────────────────────────────────────────────────
 DROP: box BYE (01030001 sel 0x00) or peer-gone budget drain ⇒ master → PROBING
```

## The five master-side fixes that made the box lock SOLID

1. **cfea box-count** (`gen_cfea`): announce `0x0001` in cfea `[20:22]` once a box is
   latched (was `0x0000`). The box must see itself acknowledged or it withholds its
   heartbeat. `[19]`=console model and `[21]`=box-count-low are DISTINCT fields (old
   code wrongly set both to console_field).
2. **ENROLL** (`cdea 01 03 000d`): the only master op reac-pw was missing (op-set diff
   vs the M-200). Emitted once on latch, before the burst; arms the box. **It is ALSO
   the audio-width gate — see "The ENROLL group map" below (2026-07-24).**
3. **Grant burst**: the master's own 32-frame `cdea 04 03` sweep, byte-exact, one-shot.
4. **Self-complete + HOLD**: after ENROLL+burst, COMMIT to ESTABLISHED and hold — the
   box goes quiet settling its TX_MUTE dwell, so waiting for a post-burst unicast (or
   timing back to PROBING) made it re-attempt forever (LED blinking faster). Do NOT
   re-grant the same box's cold-connect retry JOINs (same MAC → hold; MAC-change → re-court).
5. **Explicit box heartbeat** (`REAC_M_RX_BOX_HEARTBEAT`): the box's "I am locked"
   signal, symmetric to what our slave emits; its arrival is the definitive confirmation.

Plus the earlier **locked cadence** (cfea + chanmap each metronomic 1.00/s post-lock,
nothing else).

## Live proof
```
recognized box = S-0808 (8 in / 8 out)
GRANTING -> ESTABLISHED (timer)
rx HEARTBEAT from 00:40:ab:c4:dc:9c (state ESTABLISHED)
drops: 0    box heartbeat 1.0/s    LED SOLID
```

## The ENROLL group map — the audio-return WIDTH gate (2026-07-24)

The `cdea 01 03 000d` block does more than arm the box: **its group map tells the box
how many 8-channel input groups to open on its audio return.** In the 34-byte block
(from the `cdea` marker):

```
[8]     console-model byte (0x00 V-Mixer / 0x01 OHRCA) — the ONLY console-dependent byte
[9:14]  INPUT region:  width/8 bytes of 0x41, packed from the front
[14:19] OUTPUT region: the remaining (5 - width/8) groups as 0xc3, packed from the back
```

| box | width | [9:14] | [14:19] |
|---|---|---|---|
| S-0808 | 8 | `41 00 00 00 00` | `00 c3 c3 c3 c3` |
| S-1608 | 16 | `41 41 00 00 00` | `00 00 c3 c3 c3` |
| S-4000S | 32 | `41 41 41 41 00` | `00 00 00 00 c3` |

A pure function of width, **no per-box special case** — verified byte-for-byte across
the M-200, M-300 and M-5000 goldens (`reac-captures/captures/matrix-*.pcap`; the
console byte at `[8]` is the only difference between generations). The golden widen
sequence (`matrix-m200-s4000-coldconnect-2026-07-24.pcap`) is an ORDER, not a timing:
box CONFIG-announce (`cdea 0103 0010`, box already streaming its 8ch floor) → console
one-shot `cfea ffff 0100` → console ENROLL with the box's width map → **box widens
3 ms later** (340 B → 1204 B) → CHANMAP resumes; the JOIN → grant-burst head-amp phase
is separate and later.

reac-pw's S-4000-stuck-at-8ch bug was exactly here: a static 1×`0x41` ENROLL emitted at
GRANTING-start, ~1 ms before recognition learned the real width — the box never saw a
32-wide map (circular: enrol 8 → box streams 8 → "recognize" 8). Fix
(`set_enroll_width` + a one-shot dwell re-emit + wide-safe 32 default): the map is
derived from the recognized width, and a recognition that lands after the tick-0 ENROLL
re-emits one width-correct ENROLL inside the GRANTING dwell — the same post-CONFIG
placement the golden shows. Related, NOT the same thing: the CHANMAP sweep always
advertises the full 40-slot fabric (width-independent), and the grant burst arms
head-amps only (byte-identical first grants for S-0808 vs S-1608 on the goldens).

## Rigorously RULED OUT (do not re-chase)
- Frame length 1492 (FCS artifact). Chanmap content (49-window sweep byte-matches).
- "reac-pw isn't transmitting" — capture artifact; `tx_packets` + `sendto` prove
  ~4000 fps downstream.
- **TX timing jitter** — turned out NOT to be the issue; the gap was the PROTOCOL
  (box-count + ENROLL + hold), not the clock. #131 stays open only as a fidelity item.

## Next
1. Whole-protocol integration test: replay a real M-200 capture into our SLAVE and
   assert it reaches ESTABLISHED + heartbeats (the courtship is our-code-vs-our-code,
   so a shared wrong assumption passes it — replay-vs-real-capture would not).
2. Generalise grant burst + ENROLL + box-count width to S-1608 / S-4000S and to
   M-300 / M-5000 (from their `matrix-*.pcap`).
3. Real-MAC identity (NIC's own / set NIC to a Roland MAC) to drop promiscuous RX.
4. Drive the patches: route the box's upstream audio into PipeWire; feed the
   downstream from the graph.
