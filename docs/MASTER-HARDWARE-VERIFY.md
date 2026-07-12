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
