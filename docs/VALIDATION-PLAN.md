# reac-pw ↔ openmixer — validation plan + roadmap

Ordered plan to validate and merge **reac-pw PR #8** (per-VLAN multi-box, box-width
named nodes, rate=model) and **openmixer PR #156** (per-box stagebox UI + re-link).
Each step gates the next; merge a PR only after the step that validates it passes.

## Part 1 — Validation (in order)

### Stage A — reac-pw single box (validates PR #8 core)
1. **Unit tests** — `cd ~/Devel/audio/reacpw-wt-130 && ninja -C build && meson test -C build`.
   Gate: **12 OK, 1 SKIP** (pacer skips off-rig).
2. **setcap + establishment** — `sudo setcap cap_net_raw,cap_sys_nice+ep build/reac-pw`
   then `./build/reac-pw --live enp131s0 --role master --mixer m200 --tx enp131s0 --box s1608:S-1608`.
   Pass: log reaches `ESTABLISHED` + steady heartbeat; `tx/s ≈ 4000`.
   (Re-`setcap` after every rebuild — the link strips file caps.)
3. **Box-width nodes** — `pw-dump | grep reac`. Pass: `reac-capture` = **16** out ports,
   `reac-playback` = **8** in ports, descriptions `"S-1608 — 16 ch / 8 ch"`. (The "40 inputs" fix.)
4. **Upstream audio** — run with `REAC_DEBUG=1`, make sound into port 9.
   Pass: `reac_rx: … active_ch=16 peak=…` rises on the mic channel. → **PR #8 mergeable.**

### Stage B — box outputs (assumption to confirm)
5. Patch an openmixer bus → `reac-playback:playback_01`, feed tone, listen on box output 1.
   Pass: tone at the box out. If silent → the box output slots aren't 0..7 (roadmap: output-slot RE),
   not a blocker for inputs.

### Stage C — openmixer per-box UI (validates PR #156)
6. **Build + unit tests** — `cd ~/Devel/audio/openmixer && git checkout feat/reac-per-box-stagebox
   && pnpm -r build && pnpm -r test`. Pass: core 417, web-ui 1218, server 710.
7. **Deploy + detect** — run the #156 build as a **second instance on a spare port** (don't kill the
   live :8800 yet) with reac-pw up. Pass: patchbay shows one group "S-1608 (16 in / 8 out)", not a 40-blob.
8. **Re-link on appearance** — start openmixer FIRST, then reac-pw. Pass: saved links form
   automatically when `reac-capture` registers (no manual `pw-link`). → **PR #156 mergeable.**

### Stage D — multi-box end-to-end (needs the VLAN trunk — operator)
9. Trunk the host switch port carrying the box VLANs; per box:
   `ip link add link enp131s0 name reac.<vid> type vlan id <vid>`. Pass: `reac-pw --live reac.<vid>` sees the box.
10. One master per VLAN: `reac-pw --live reac.10 … --box s1608:Drums --name drums`,
    `… reac.20 … --box s0808:Vocals --name vocals`. Pass: `reac-capture.drums`(16) + `reac-capture.vocals`(8)
    coexist, both boxes lock.
11. openmixer shows two named groups at real widths, both patch/audio independently. → **multi-box validated.**

**Merge order:** #8 (reac-pw) first, then #156 (openmixer depends on the reac-pw node contract).

## Part 2 — Roadmap (pending development)

**reac-pw master**
- **96 kHz OHRCA emit** — 1494-byte frames (RE the 2-byte trailer/CRC), chanmap `fe 01` + cfea console `01`,
  pace 8000 fps. The only path to real 96 kHz (rate is the model, not `--rate` — see MASTER-HARDWARE-VERIFY.md).
- **Box output-slot mapping RE** — which downstream fabric slots a box reads as its outputs (Stage B).
- Peer-gone / re-establish hardening for a box that drops mid-show.

**reac-pw slave (impersonate a box to a real desk)**
- **#133 frame-locked upstream TX + #131 clock discipline / repacer** — the M-5000 grants our slave but
  never goes fully LINKED; the last gap is the upstream timing/jitter lock. Biggest remaining slave item.
- **#132** PipeWire `reac:return` sink (inject audio as the box's mic inputs).
- **#135** per-generation downstream decode (M-5000 plain-LE vs M-200/M-300 braid; also fixes reac-aes67
  `reac_decode` still being plain-LE).

**openmixer**
- Stagebox card output control + `assignGroup` wire clamp (#156 left out of scope).
- **#128** EQ id-based drag identity. Roll #156 out to the live instance.

**Infra / bigger**
- **#32** REAC→AES67 bridge appliance. **#18** `--etf/SO_TXTIME` egress pacing (i226).
