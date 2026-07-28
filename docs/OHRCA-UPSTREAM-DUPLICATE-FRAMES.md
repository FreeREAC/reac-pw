# OHRCA upstream frame duplication (the "granulated audio" bug)

**Status:** audio FIXED 2026-07-25 (operator-confirmed clean); mechanism CORRECTED
same day by the operator. The dedup half lives in `src/reac_rx.c`; the trailer-strip
half moved into libreac on 2026-07-28 (`87297ca`) and is now
`<reac/reac_upstream.h>` / `reac_frame_clean_len` — there is no `src/reac_upstream.c`
in this repo.

> **CORRECTION (operator, 2026-07-25):** REAC stageboxes send each frame ONCE,
> slaved to the master clock. The duplication was NOT the box re-transmitting —
> it was the **RX path**: reac-pw was mastering on the USB NIC
> (`enp128s20f0u6`), which hangs off a **mirrored switch port** (originally used
> only to sniff the protocol). The mirror delivers every frame twice — both the
> box's upstream AND our own downstream echoed back. The decisive evidence was
> in the measurements all along, initially misread: the pacer emits **4000 fps**
> yet our OWN TX frames were captured at **8000 fps** — a box cannot double our
> frames; a port mirror does, in both directions. Every "the box streams 96 kHz"
> / "doubled cadence" claim below was a mirror-doubled COUNT: the box ran 48 kHz
> the whole time. The byte[14:15] counter "incrementing every 2 frames" is the
> same frame seen twice, not two transmissions.
>
> Consequences:
> - The **dedup guard stays** — it is what made the audio clean, and it is the
>   right robustness guard against any duplicated-delivery path (mirror ports,
>   bridged captures). On a clean NIC it is a strict no-op (`dup=0`).
> - The **proper remedy** is to master on a non-mirrored port (the PCIe
>   `enp131s0` per the rig map). Verification that `dup=0` there is PENDING: at
>   the time of writing the USB NIC had just vanished from the system and the
>   box was not yet establishing via `enp131s0` (our broadcast is on that wire,
>   the box's probe frames appear, but no JOIN completes) — re-check cabling.
> - The +2 CRC-trailer finding is about frame CONTENT and stands unaffected.

The original analysis follows, kept for the measurements; read its box-behaviour
claims through the correction above.

---

## Symptom

A real stagebox's mic audio arriving through reac-pw sounded **granular / stuttery**
("granulated sound") even though phantom, establishment, and metering all worked.
The symptom appeared during the head-amp / 96 kHz-OHRCA work (#155/#156); the same
box had sounded clean before, when reac-pw ran the segment at 48 kHz.

It is **not** a decode bug. Every individual 24-bit sample decodes correctly — the
braid byte-order (even `g[3],g[0],g[1]`, odd `g[4],g[5],g[2]`), the audio-start
offset (50), and the OHRCA +2 CRC trailer handling are all right. The corruption
was in **frame assembly**.

## Root cause: the box transmits every frame TWICE, byte-identical

When a 48 kHz-native box (S-4000S, S-1608, …) is driven at the **96 kHz OHRCA
doubled cadence** (8000 fps instead of 4000), it fills the doubled slot count by
**re-transmitting each 48 kHz frame verbatim**. Measured on the wire (box src
`00:40:ab:c4:08:bc`, NIC `enp128s20f0u6`):

- Header **byte[14:15]** is a free-running counter that **increments every 2 frames**
  (e.g. `21,21,22,22,23,23 …`).
- The two frames sharing a counter are **100 % byte-identical** (15878/15878
  adjacent pairs in a 4 s capture), **~125 µs apart** — a genuine second
  transmission, *not* a capture mirror (tcpdump reported 0 kernel drops; the
  inter-frame gap is a uniform 124.9 µs, and there are no sub-10 µs "mirror pairs").
- Adjacent *samples* are **not** duplicated (0.4 %), so it is frame-level, not
  sample-level, repetition.

reac-pw's RX fed **both** copies into the ring, producing
`[A0..A11][A0..A11][C0..C11][C0..C11]…` — every 12-sample block played twice. That
is the granular per-frame stutter, and it also **doubled the effective sample rate**
(96 kHz of data into a 48 kHz `reac-capture` node → ring overrun), which earlier
looked like a standalone "rate mismatch" but was really a symptom of the doubling.

## Proof it is duplication, not decoding

The same wire capture, same braid decode, assembled two ways:

| assembly | result |
|---|---|
| **DEDUP** — one frame per byte-identical pair (→ true 48 kHz) | clean |
| **CONCAT** — both copies (the buggy behaviour) | the granular stutter |

Operator confirmed the DEDUP audio is clean and CONCAT is the known symptom.

A separate 4-angle adversarial check exonerated the decoder: the braid HI byte
`g[2]` is 100 % sign-extension (`0x00`/`0xFF`) — a true 24-bit MSB — whereas plain-LE
has **no** true MSB (0.131 sign-dominance) and is provably wrong; ±2-byte audio-start
shifts collapse to full-scale noise; per-frame phase energy is flat (ratio 1.007),
so no end-marker/CRC leaks into the audio.

## Fix

`src/reac_rx.c` (RX ingest, before counter/ppm/decode): **drop a frame byte-identical
to the one immediately before it.** Safe and self-adapting:

- Identical frames carry no new audio, so dropping the repeat is lossless.
- Genuine distinct frames (32 ch × 12 samp × 24-bit) are never byte-identical, so a
  true 48 kHz box (no duplication) and a real distinct-frame 96 kHz source are both
  unaffected.
- Restores the true 48 kHz cadence into `reac-capture`, so the rate estimator and
  ring see the real rate.

libreac's upstream decode (`<reac/reac_upstream.h>`, was `src/reac_upstream.c` here
until 2026-07-28): strip the OHRCA **+2 CRC-16 trailer** (`1206 → 1204`,
`52 + 32·36 + 2`). Required — without it the S-4000 frames fail the `%36` width
check and `reac-capture` is silent. Confirmed a real box field, not the Ethernet
FCS: in one capture reac-pw's own downstream frames are all 1492 B (no +2) while the
box's upstream are all 1206 B (+2). The box mixes ~1/8 frames at 1204 (no +2); both
resolve to 32 ch.

### Verification

`REAC_DEBUG=1` telemetry gains a `dup=` counter. Live on the S-4000S: `dup ≈ ok`
(~50 % of frames dropped = the duplicates), the ring `fill` stops climbing, and audio
flows where it previously overran. The standalone `pw-record --target reac-capture`
reads silent regardless (that node only delivers through openmixer's graph), so the
audible check is openmixer with the live condenser.

## Related

- `#156` — 96 kHz OHRCA emit (same FSM, doubled cadence): the cadence change that
  triggered the box's duplication.
- `#160` — reac-pw↔openmixer graph-rate coupling (the deeper "drive the box at the
  rate the mixer wants" work); dedup is the robust RX-side guard regardless.
