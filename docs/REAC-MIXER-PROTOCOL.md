# reac-pw as a MIXER (master) — establishment protocol (RE, 2026-07-12)

Goal: reac-pw impersonates a real Roland desk (M-200 first, then M-300/M-5000) so
a real stagebox slaves to **it**. The establishment automaton is the SAME state
diagram as the box side (see `REAC-BOX-STATE-DIAGRAM.md`) — the box's frames are
the transition events; here they drive the MASTER's state.

## Status: box RESPONDS + reac-pw ESTABLISHES + RECOGNISES; box not yet LOCKED

Live against a real S-0808 on an M-200-class rig (2026-07-12):
```
box presence GAINED (UNICAST from c4:dc:9c)
recognized box = S-0808 (8 in / 8 out)     ← recognizer #137, live
PROBING -> ESTABLISHED (rx CONFIG)         ← warm-relink establishment
```
The box's status light still **BLINKS** (not solid) — the last gap is the master
grant/ack sweep (below).

## What made a real box respond — 4 fixes (all committed, FreeREAC/reac-pw, branch design/slave-emulation-scope)

1. **M-200-exact probe specials** (75fe762). The master's inventory-special probes
   (MAC / SYSP / SCEN) were M-300-derived and **SYSP was byte-shifted +1** (block
   idx 24 vs the M-200's 23). Byte-matched to a real M-200 (`m200-s1608-establish`,
   `c9:cc:03`). This was THE unlock: a box that was **totally silent** to reac-pw's
   master now recognises it as a mixer. The box gates on the master's FULL probe
   repertoire — a 15-variant cycle including MAC-bearing, "SYSP", "SCEN" specials
   plus the 02/03 rotation.
2. **Promiscuous RX** (97eae22). reac-pw TXes with a SPOOFED Roland-OUI src MAC,
   not the NIC's hardware MAC. A box establishes by UNICASTING its config-announce
   + upstream to that spoofed MAC — which the NIC's hardware filter drops. Without
   `PACKET_MR_PROMISC` on the pacer RX socket the master saw `rx_box_frames=1`
   while the box streamed 31.9k frames (seen on the mirror). **Design alternative
   (preferred, TODO): use the NIC's REAL hw MAC as the master identity to avoid
   promisc entirely** — and it doubles as the Roland-OUI-validation test.
3. **Warm-relink establishment** (1e0f65b). A previously-synced box does a WARM
   RELINK: it skips flood + cold-connect and re-appears streaming unicast,
   re-declaring itself ONLY via config-announce (`cdea 01 03 0010`), never a
   `04 03` JOIN. Added `REAC_M_RX_BOX_CONFIG`; PROBING/GRANTING + CONFIG ->
   ESTABLISHED (a specific checksum-valid frame, not mere presence).
4. **Recognizer** (8df3bd2, #137). `reac_ctrl_identify_box()` names the box from
   that same config-announce — live "recognized box = S-0808".

## THE REMAINING PIECE — the grant/ack sweep (flips the light blinking → SOLID)

A real M-200, once a box is present, **continuously emits a GRANT/ACK sweep**: a
sequence of ~32 distinct `cdea 04 03 0013` blocks (plus a couple of `04 03 0014`)
iterating channel/slot, with `12 12` / `12 11` bank variants. Fixed 16-byte header
`04 03 00 13 00 02 00 fe 0e f0 41 0a 00 00 12 1x`, then an iterating tail
`01 01 <bank> <ch> <x> <cksum> f7 …`. **Cadence: once per second** (Pau's
recollection, documented) — the full ~32-block sweep repeats ~1/s.

reac-pw only emits a grant while ECHOING a JOIN (the GRANTING window); a
warm-relinking box sends no JOIN, so reac-pw never grants and the box never locks.

**Fix (in progress):** add a `GRANT_STREAM` (the 32 byte-verified M-200 blocks,
extractable from `matrix-m200-s0808-2026-07-11.pcap`, box locked, master
`c9:cc:03`) and emit it from the ESTABLISHED control cadence (`control_cadence()`
in `reac_master.c`), cycling once per second, byte-exact. This is the thing that
takes the box from blinking → solid = a complete mixer protocol.

## Rigorously RULED OUT (do not re-chase)
- **Frame length**: 1492 B is correct. The "700 B" was capture snaplen; the
  "1494 B" was the **Ethernet FCS** captured by a mirror config (see
  `SLAVE-EMULATION-SCOPE.md` — the FCS-trailer falsification).
- **Chanmap**: reac-pw's 49-window sliding sweep already byte-matches the M-200 —
  the earlier "chanmap differs" was comparing different cursor positions of the
  same sweep.

## Next, for a COMPLETE mixer protocol
1. Grant/ack sweep (above) → solid lock. **The finish line.**
2. Real-MAC-as-identity to drop promisc + confirm OUI validation.
3. Drive the patches: allocate the recognized model's in/out width into the master
   I/O; route the box's upstream audio into PipeWire.
4. Full cold-connect JOIN path (fresh cold-boot box sends `04 03` JOINs) — verify
   the classifier accepts real S-0808/S-1608 JOIN blocks and grants.
5. Generalise the probe specials to M-300 / M-5000 (impersonate any desk).
