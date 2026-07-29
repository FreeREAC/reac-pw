# Master role — hardware-verify status

reac-pw as a Roland V-Mixer/OHRCA desk, driving real stageboxes. Verified on
the bench against a live **S-1608** (`00:40:ab:c4:80:3b`) on 2026-07-12.

## What is rig-verified WORKING

Run (48 kHz V-Mixer profile; re-`setcap` after every rebuild — the link strips it):

```
sudo setcap cap_net_raw,cap_sys_nice+ep ./build/reac-pw
./build/reac-pw --live enp131s0 --role master --rate 48000 --mixer m200 --tx enp131s0
```

- **Establishment.** The box floods its presence, cold-connects (`cdea 04 03`
  escalating `0014→0013→0016→001a`), reac-pw recognizes the model, grants, and the
  box locks: `PROBING → GRANTING → ESTABLISHED`, steady 1/s box heartbeat. Solid.
- **Upstream audio.** The box's **16 mic channels are decoded and delivered to the
  `reac:capture` PipeWire source** (one output port per fabric slot; the box's
  inputs land at ports 1..16). The braided box-width return (`reac_upstream_decode`,
  628 B / 16 ch) is byte-correct — confirmed by decoding a live frame and by the
  in-process telemetry (`active_ch=16`). In a quiet room the level sits at mic
  self-noise (~-90 dBFS); it rises with real input.

### Observability — `REAC_DEBUG`

`REAC_DEBUG=1 ./build/reac-pw …` enables two opt-in telemetry streams on stderr
(both OFF by default, no hot-path cost):

- `reac_rx: ok=N other=N bad=N gaps=N src=…` (~2 s) — the wire→ring decode.
  `other` climbing = the stream gate is rejecting the box (wrong src lock / shape);
  `ok` climbing with `other=0` = the box return is decoding cleanly.
- `reac_src: nframes=… linked=… active_ch=… peak=… fill=…` (~1 s) — the
  ring→PipeWire read. `active_ch` = channels carrying signal; `fill` = ring backlog.

## End-to-end through openmixer — VERIFIED

Linked the box's 16 `reac-capture:capture_NN` ports into openmixer's native console
(`omx-console:in_N`, `OPENMIXER_CONSOLE=32:16`). The ring `fill` holds STABLE at
~30–40 samples with `active_ch=16` and live box audio — it does NOT drain to zero.
So **openmixer's engine rate-matches the REAC clock properly and the box's mic
channels reach the console cleanly.** The stagebox is usable end-to-end.

## Known gaps / next steps

1. **Ring underrun is a dumb-consumer artifact, not a real bug.** With `pw-record`
   (a same-rate consumer that ignores `io_rate_match`) the ring `fill` drains to zero
   → glitches. With openmixer's real graph it stays stable (above). The follower
   rate-match (`ppm_error → io_rate_match.rate`) is honoured by a consumer that
   resamples/adapts. Only pursue the pacer buffer-servo / reac-pw-as-graph-driver
   (#131) if a real consumer is shown to glitch; openmixer does not.

2. **openmixer link ordering.** openmixer's saved patch links `reac-capture ↔
   omx-console ↔ reac-playback`, but if openmixer starts BEFORE reac-pw its links
   fail ("endpoint port(s) not registered") and it silences retries — it does NOT
   re-establish them when the reac nodes appear later. Start reac-pw first, or teach
   openmixer to (re)link on reac node registration (its #68 re-route-on-recreate
   class). Manual bridge meanwhile: `for i in $(seq -w 1 16); do pw-link
   reac-capture:capture_$i omx-console:in_$((10#$i)); done`.

2. **Second box / multi-box.** A real **S-0808** (`00:40:ab:c4:dc:9c`) on the same
   segment stays silent until it PHY-links while reac-pw streams (power-cycle /
   replug it — a box cold-connects on link-up, not on seeing a master mid-stream).
   Even then, the master FSM tracks ONE box and the RX gate locks to ONE src MAC, so
   the second box's return is gated out. Multi-box = a per-box FSM + non-overlapping
   fabric-slot allocation + a multi-source RX lane (the 40-slot allocation).

## Gotchas (do not re-derive)

- **The USB SPAN mirror (`enp128s20f0u6`) adds the 2-byte Ethernet FCS** — box audio
  shows as **630 B** there but is **628 B** on reac-pw's own NIC (`enp131s0`,
  post-FCS-strip). Diagnose frame *length* on the participant NIC, not the mirror.
- A local `AF_PACKET` RX does NOT see the host's own TX; verify reac-pw's downstream
  via `/sys/class/net/enp131s0/statistics/tx_packets` or the mirror.
- Promiscuous mode is mandatory (the box unicasts to the spoofed master MAC, not our
  HW MAC); the pacer sets it device-wide, so all AF_PACKET sockets receive the box.

## Sample rate is the desk MODEL, not a clock knob (RE 2026-07-13)

Diffed a real M-5000 (96 kHz) vs M-300 (48 kHz) downstream (`real-s1608-coldboot-m5000`
vs `m300-s1608-establish`):

| | M-5000 (96 kHz) | M-300 (48 kHz) |
|---|---|---|
| frame (as captured) | **1494 B (OHRCA)** | **1492 B (V-Mixer)** |
| cfea console byte | `28 10 **01** …` | `28 10 **00** …` |
| chanmap marker | `fe **01**` | `fe **00**` |

There is **no explicit 48000/96000 field** anywhere. The box infers its rate from the
desk IDENTITY: OHRCA (`01` cfea/ENROLL console markers) ⇒ 96 kHz; V-Mixer (`00`) ⇒
48 kHz — see "The 1494-byte frame is a capture artifact" below for why the frame
LENGTH row above is not itself part of that signal.

### 96 kHz OHRCA emit — DONE (task #156, 2026-07-14): parameterized, not re-engineered

"96k is not anything different, same state diagram, doubled frequency." The
downstream frame SHAPE does not vary by mixer profile or rate (see the trailer
finding below), so 96 kHz needed no new emit path — only the two things a real
OHRCA desk actually varies:

- **Pacer cadence** — fps = rate/12 (`reac_sink_node.c`), already rate-driven;
  `--mixer m5000` + `--rate 96000` now yields 8000 fps, `--mixer m200/m300` stays
  4000 fps. `reac_master_init`'s cadence math (`cycle_len`, `chanmap_off`, …) scales
  purely off fps for either profile — no 48k assumption was baked in there.
- **Console identity** — cfea `[19]`/ENROLL `[8]` = `console_field`, already threaded
  from `--mixer` through `reac_sink_cfg`/`reac_pacer_cfg`/`reac_master`.

The only real gap was `main.c`'s `--rate` handling, which force-clamped to 48 kHz
**unconditionally**, for every profile including `m5000`. Fixed via
`reac_mixer_resolve_rate()` (`reac_master.h`/`.c`).

⚠ **SUPERSEDED 2026-07-28 (#73, a183296 + 71e1a1d).** This section originally said
that V-Mixer profiles (`m200`/`m300`, `console_field == 0`) keep an unconditional
48 kHz clamp because "a V-Mixer desk has no wire rate field, so it can only ever run
48 kHz", and that OHRCA (`m5000`) defaults to its native 96 kHz. **Neither survives.**
Both were inferences from the console identity byte, never demonstrated: the byte
says which desk we are impersonating, not which rate the operator chose. On a real
Roland desk the operator picks the REAC rate from a menu and every box follows —
reac-pw IS the master, so `--rate` is that menu and there is nothing to clamp it
against. `reac_mixer_resolve_rate()` now honours the request for every profile and
defaults to 48 kHz (the working standard for live work) when `--rate` is unset; the
reasoning is written out at `src/reac_master.c:185-210`. It is kept as a function,
not deleted, so a real demonstrated rule would have one obvious home.

Covered by `tests/test_reac_master.c` (resolve-rate table + fps mapping + frame-size
invariance) and `tests/test_reac_tx.c` (the trailer finding below).

Run 96 kHz — `--rate` is **required**, no profile implies it:
```
sudo setcap cap_net_raw,cap_sys_nice+ep ./build/reac-pw
./build/reac-pw --live enp131s0 --role master --mixer m5000 --rate 96000 --tx enp131s0
```

### The 1494-byte frame is a capture artifact, not a REAC field (task #156 RE)

The "OHRCA trailer" — the 2 extra bytes that make some M-5000 downstream captures
1494 B instead of 1492 B — is **not a REAC-level field**. Sampled 1494-byte
downstream frames from two independent real-M-5000 captures
(`real-s1608-coldboot-m5000-2026-07-11.pcap`, `s4000s-coldboot-m5000-2026-07-12.pcap`,
40 frames total, `reac-captures/captures/`): in every case the trailer equals the low
16 bits (little-endian) of the standard Ethernet CRC-32 (IEEE 802.3) over
`frame[0:1492]`. The second capture is a dual-tap/BIDIR-style recording that caught
the SAME wire frame TWICE — once at 1492 B, once at 1494 B, byte-identical in
`[0:1492]`, same counter — which proves the extra 2 bytes are a mirror/SPAN capture
artifact (partial Ethernet FCS passthrough — see the existing rig gotcha above, "The
USB SPAN mirror adds the 2-byte Ethernet FCS"), not something the desk's REAC logic
emits. This is the same conclusion already reached and committed for the box-
UPSTREAM direction (`docs/SLAVE-EMULATION-SCOPE.md` W4(a), 2026-07-12); this is the
independent reproduction for the master's DOWNSTREAM direction.

Consequence: there is **nothing to crack and nothing to emit**. A real desk's actual
wire frame is `REAC_FRAME_BYTES` (1492) plus whatever 4-byte FCS its own NIC hardware
appends — identical in kind to every other Ethernet frame reac-pw already sends over
`AF_PACKET`. Emitting a literal 1494-byte payload would not reproduce a real desk's
frame; it would put 2 extra GARBAGE bytes into the payload before the NIC's own real
FCS, actively breaking on-wire correctness. `reac_eth_crc32()` (`reac_tx.h`/`.c`) is
kept as a pure, documented verification utility only (reproduces the captured trailer
in `tests/test_reac_tx.c`) — it is never called from the encode path.

### On-wire validation gate (still open)

Everything above is proven from captures + unit tests (`meson test`: all green, the
only SKIP being `reac_pacer`'s live-cadence case, which needs `CAP_NET_RAW`; the
suite has grown from 14 tests at the time of this task to 29, so read the count off
`meson test -C build`, not from here). Attempted a disposable `--live lo` self-test
(tcpdump on `lo`,
`--mixer m5000 --rate 96000`, `setcap cap_net_raw,cap_sys_nice+ep`) to eyeball frame
sizes on the wire; blocked by the dev sandbox lacking `CAP_NET_RAW` even after
`setcap` (the same reason `tests/test_reac_pacer.c`'s live-cadence case SKIPs there).
**Not yet validated against real hardware.** Before calling 96 kHz OHRCA emit
hardware-proven, run on the bench rig against a real M-5000, or an S-1608 forced to
96 kHz by a real M-5000 upstream of it:

```
sudo setcap cap_net_raw,cap_sys_nice+ep ./build/reac-pw
./build/reac-pw --live enp131s0 --role master --mixer m5000 --tx enp131s0
```

and confirm: (1) the box cold-connects and reaches ESTABLISHED exactly as the 48 kHz
`m200` path does (same FSM, per the "do not re-engineer" framing — no new establishment
behaviour is expected); (2) `tcpdump -i enp131s0 'ether proto 0x8819'` shows reac-pw's
downstream frames at exactly 1492 B, 8000 fps; (3) the box's own upstream return runs
at 8000 fps (96 kHz), not 4000 — the mismatch class this task exists to fix (see the
2026-07-13 RE note above, "the box streamed 48 kHz upstream while our pacer ran
8000 fps"). Until that is checked off, treat 96 kHz OHRCA emit as protocol-level-sound
but hardware-unverified.
