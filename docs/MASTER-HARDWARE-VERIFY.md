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

## Known gaps / next steps

1. **Ring underrun (`fill` drains below one quantum).** The RX producer is paced by
   the wire on `CLOCK_MONOTONIC`; the graph consumer runs on the graph clock. Same
   nominal rate, slow relative drift, minimal buffering → periodic underruns
   (glitches) on a same-rate consumer that does not honour `io_rate_match` (e.g.
   `pw-record`). The follower rate-match (`ppm_error → io_rate_match.rate`) corrects
   long-term drift only where a resampler sits on the link. Real fix = the pacer
   clock-discipline / buffer servo (see the DLL clock-discipline task). Re-test in
   openmixer's actual graph before assuming it glitches there.

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
