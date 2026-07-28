# reac-pw ↔ openmixer — validation plan + roadmap

Ordered plan that validated and merged **reac-pw PR #8** (per-VLAN multi-box,
box-width named nodes) and **openmixer PR #156** (per-box stagebox UI + re-link).
Each step gated the next; a PR merged only after the step that validated it passed.
**Both are merged** — Part 1 is kept as the procedure (it is the re-test protocol,
and Stage B carries a two-day mis-diagnosis written up so it is never repeated),
Part 2 is the live roadmap.

One claim from PR #8's title did not survive: "rate = model". The console identity
byte says which desk we impersonate, not which rate the operator chose — see #73
and `MASTER-HARDWARE-VERIFY.md`'s superseded-rate note.

## Part 1 — Validation (in order)

### Stage A — reac-pw single box (validates PR #8 core)
1. **Unit tests** — `ninja -C build && meson test -C build`.
   Gate: **all green, one SKIP** — `reac_pacer`'s live-cadence case, which needs
   `CAP_NET_RAW` and so skips off-rig. (Do not gate on a count: it was 12 tests when
   this plan was written and is 29 now. `meson test` reports the total itself.)
2. **setcap + establishment** — `sudo setcap cap_net_raw,cap_sys_nice+ep build/reac-pw`
   then `./build/reac-pw --live enp131s0 --role master --mixer m200 --tx enp131s0 --box s1608:S-1608`.
   Pass: log reaches `ESTABLISHED` + steady heartbeat; `tx/s ≈ 4000`.
   (Re-`setcap` after every rebuild — the link strips file caps.)
3. **Box-width nodes** — `pw-dump | grep reac`. Pass: `reac-capture` = **16** out ports,
   `reac-playback` = **8** in ports, descriptions `"S-1608 — 16 ch / 8 ch"`. (The "40 inputs" fix.)
4. **Upstream audio** — run with `REAC_DEBUG=1`, make sound into port 9.
   Pass: `reac_rx: … active_ch=16 peak=…` rises on the mic channel. → **PR #8 mergeable.**

### Stage B — box outputs

5. Patch an openmixer bus → `reac-playback:playback_01`, feed tone, listen on box output 1.
   Pass: tone at the box out. If silent → the box output slots aren't 0..7 (roadmap: output-slot RE),
   not a blocker for inputs.

**Result: PASSED 2026-07-13 (braid encode, listen-confirmed) — after a two-day
mis-diagnosis loop recorded here in full so it is never repeated.**

**The correct downstream encode is the REAC BRAID** (obs-h8819 even/odd
channel-pair layout, `reac_tx.c` default), confirmed by four independent lines:

1. **reacdriver** (per-gron, macOS): its to-device conversion is a 16-bit word
   byte-swap of big-endian s24 host PCM (`MbufUtils.cpp`: `out = in[1],in[0],
   in[3],in[2],in[5],in[4]` per channel pair) — **byte-identical to the braid**
   (asserted structurally in `test_reac_tx`). This is the "LE bytes swapped"
   fix the operator remembered.
2. **obs-h8819** (norihiro): `convert_to_pcm24lep`, developed and
   listening-validated against a **real Roland M-200i downstream** at 48 kHz —
   our exact console generation.
3. **Our rig**: the S-1608/S-0808 **upstream** return uses the same braid,
   validated with real microphones (#108); downstream is symmetric.
4. **Goldens**: zoneA/zoneB (a real M-5000's two REAC ports, program audio)
   decode at **coherence 0.99 / spectral flatness 0.002** under the braid at
   audio offset **exactly 50**, and as noise under every other layout × offset
   (table below). Listen-confirmed on our S-1608: console program through
   `reac-playback:playback_08` is clean on the braid build.

Coherence table (rms-weighted lag-1 autocorrelation / spectral flatness /
full-scale-noise channel count; 3000 downstream frames each; audio offsets
50+0..4 scanned, winning offset shown):

| capture | layout | coherence | flatness | FS-noise ch |
|---|---|---|---|---|
| zoneA-48k | braid @50 | **0.988** | **0.0015** | 0 |
| zoneA-48k | plain-LE @50 | 0.234 | 0.4259 | 1 |
| zoneA-48k | wordswap16(LE host) @50 | 0.001 | 0.5604 | 4 |
| zoneB-48k | braid @50 | **0.981** | **0.0019** | 0 |
| zoneB-48k | plain-LE @50 | 0.236 | 0.4321 | 1 |
| zoneB-48k | wordswap16(LE host) @50 | −0.004 | 0.5641 | 4 |

**How the mis-diagnosis loop happened (do not repeat it):**

- The original "noise on port 8" complaint was a **plain-LE build**: a
  de-braiding box plays every plain-encoded output as a ~−42 dBFS mid/hi-byte
  hash of the program — "right-ish level, garbage content". 895afb9 (braid)
  fixed it; the operator's clean window was that build.
- A −20 dBFS sine test on the braid build then produced a violent near-full-scale
  **burst** at box output 8, which was misattributed to the braid, and the
  encode was reverted to plain-LE (e09cb31 / PR #13 first version) — bringing
  the garbage back. **The burst does NOT reproduce on the wire**: replicating
  the exact procedure (pw-play mono wav, manual `pw-link output_FL →
  playback_08`, running braid master, isolated veth capture) yields a clean
  440 Hz at −39 dBFS on slot 7 (99.6% band energy), perfect counters, all other
  39 slots exactly silent — the manual stream→DSP-port link negotiates
  correctly. Remaining burst suspects, in order: (a) the **3-link sum** into
  `playback_08` (sine + console out_L + out_R + at times a
  `reac-capture:capture_01` loopback) — the pile was wire-measured **clipping
  at 1.00000 peak**, and a box-in1→out8 loopback is a feedback bomb; (b) the
  box's **clock PLL re-lock transient** after a master restart (see protocol);
  (c) session **stream volume**: both wire tests measured a constant −9 dB
  soft-volume on the pw-play path — a unity-volume session plays 9 dB hotter
  than expected.
- The "plain-LE is rig-validated on a live M-5000" evidence (reac-aes67
  e2e82ac, "coherence 0.999") is **contested by the zoneA/zoneB goldens from
  the same desk**: on quiet braided content the plain decode's mid→hi lane
  shift amplifies the signal 256× into a louder, perfectly coherent-looking
  image (zoneA plain-ch7 = 0.0005 rms braided audio × 256 = the 0.144 rms
  "coherent" reading; plain-ch13 = the uniform mid-byte noise of braided
  program, entropy exactly 4.000/4.000 bits), while the correct braid decode of
  quiet content looks like a noise floor. Wrong-layout decodes can look BETTER
  than the truth on quiet material — always fingerprint with program-level
  audio and check for the 256×/uniform-byte signatures. #135 should
  re-validate a real M-5000 with loud program before keying the encode per
  mixer profile (the `REAC_TX_LAYOUT=plain` A/B override was removed after
  the braid was confirmed; the plain layout survives as the negative control
  in `tests/test_reac_tx.c`).

**Stage B re-test / listen protocol:**

1. Wire-first, always: capture the master TX (SPAN mirror; the mirror delivers
   each frame twice, 1492 B + 1494 B — dedupe by counter) and braid-decode ALL
   40 slots: the fed slot must be a clean tone at the expected level and every
   other slot exactly silent, BEFORE any monitor is enabled.
2. Unlink everything from the box-output port under test except the one test
   source (`pw-link -d` the console/loopback links; restore after). Never leave
   a `reac-capture:capture_NN → reac-playback:playback_NN` loopback in place
   during output tests.
3. After any master restart, wait for the box to re-lock its clock PLL (SYNC
   LED steady) beyond protocol ESTABLISHED before judging audio.
4. Source at −30/−40 dBFS, monitor at minimum, hand on the power switch; expect
   the session soft-volume (~−9 dB observed) in level judgements.
5. The historical plain-LE garbage symptom is reproduced offline by the
   negative control in `tests/test_reac_tx.c` (the runtime `REAC_TX_LAYOUT`
   A/B override was removed once the braid was confirmed).

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
- **96 kHz OHRCA emit — DONE (task #156, 2026-07-14)**, parameterized off the existing
  48 kHz path (fps = rate/12, console_field already wired); the frame stays 1492 B —
  the "1494-byte OHRCA frame" plan below was based on a mirror-capture artifact,
  RE'd and falsified this task. See MASTER-HARDWARE-VERIFY.md's "96 kHz OHRCA emit"
  + "1494-byte frame is a capture artifact" sections. Still needs the on-wire
  validation gate documented there (real M-5000 / S-1608-at-96k).
- **Box output-slot mapping RE** — which downstream fabric slots a box reads as its outputs (Stage B).
- Peer-gone / re-establish hardening for a box that drops mid-show.

**reac-pw slave (impersonate a box to a real desk)**
- **#133 frame-locked upstream TX + #131 clock discipline / repacer** — the M-5000 grants our slave but
  never goes fully LINKED; the last gap is the upstream timing/jitter lock. Biggest remaining slave item.
- **#132** PipeWire `reac:return` sink (inject audio as the box's mic inputs).
- **#135** per-generation downstream decode. NOTE (2026-07-13, post-Stage-B): the
  **braid is confirmed for the V-Mixer-generation box pairing** (listen-proven on our
  S-1608 + obs-h8819's real-M-200i validation + reacdriver's wordswap16(BE) to-device
  conversion, which is byte-identical to the braid). The "M-5000 = plain-LE" claim
  (reac-aes67 e2e82ac) is **contested**: the zoneA/zoneB goldens from the M-5000's own
  REAC ports decode braided, and the plain "coherence 0.999" is explained by the
  mid→hi lane shift amplifying quiet braided audio 256× into a coherent-looking image
  (see Stage B). Re-validate a real M-5000 with LOUD program before adding a
  mixer-profile-keyed encode; until then the braid is the only encode (the
  `REAC_TX_LAYOUT=plain` runtime override was removed — the plain layout lives on
  as the `tests/test_reac_tx.c` negative control). This also means
  reac-aes67's `reac_decode` plain de-interleave likely needs the same braid fix.

**openmixer**
- Stagebox card output control + `assignGroup` wire clamp (#156 left out of scope).
- **#128** EQ id-based drag identity. Roll #156 out to the live instance.

**Infra / bigger**
- **#32** REAC→AES67 bridge appliance. **#18** `--etf/SO_TXTIME` egress pacing (i226).
