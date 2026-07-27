---
name: reac-control-plane-re
description: Reverse-engineer and rig-test REAC 0x8819 control-plane changes (reac-pw master vs a real Roland stagebox/M-200) — frame layout, pcap parsers, the build→launch→self-churn test loop, and the hard-won discipline. Use for any reac-pw head-amp / establishment / handshake work against real hardware.
---

# REAC control-plane RE + rig testing

The disciplined, repeatable way to change reac-pw's control plane and verify it against a
real Roland stagebox (S-1608/S-0808/S-4000) on the wire. Follow this exactly every time.

## Non-negotiable discipline (learned the hard way)

1. **Measure, never theorize.** Every claim about box behaviour must come from a pcap or the
   FSM log, not reasoning. The operator has killed many plausible-but-wrong hypotheses;
   default to a wire capture before believing anything.
2. **Never mask a real desync.** If the box is cycling/un-syncing, do NOT extend timeouts /
   fake presence to make reac-pw *report* "established" — that makes reac-pw lie. Fix the cause
   or report honestly. (Diagnostic knobs that mask: `REACPW_LINKCHECK_SECONDS`,
   `REACPW_AUDIO_KEEPALIVE` — default OFF, do not ship.)
3. **Follow the M-200 order.** The real M-200 is the golden reference. Match its establishment
   ORDER + bytes; don't invent frames it never sends (e.g. it sends NO enroll, NO SUB01/02).
4. **No hardware claim from a soft meter.** 48V "committed" ⇒ a *condenser mic comes alive*
   (needs real phantom); an LED or register readback is not proof. Dynamic mic = positive control.
5. **One change per rig cycle.** Stack knobs only to isolate; record what each did.

## Rig facts (verify each session — they drift)

- NIC to the box: `enp128s20f0u6` (USB). Was historically "sniff-only" but currently the live
  master NIC — always re-confirm with a 2 s tcpdump.
- MACs: reac-pw master `00:40:ab:c9:cc:04`; real M-200 `00:40:ab:c9:cc:03`; S-1608 box
  `00:40:ab:c4:80:41`.
- Single-master rule: only ONE master on the segment. To capture the M-200, unplug reac-pw's
  role (kill it) AND confirm `cc:03`/`cc:04` presence with tcpdump before launching the other.
- The box **self-churns** (~8 s re-cold-connect cycle in the current no-enroll build) — this is
  a FREE cold-connect test loop: change reac-pw, relaunch, and the box hits the new
  establishment on its own within ~8 s. No operator power-cycle needed for FSM-stability tests.

## Frame layout — MASTER downstream control (absolute pcap-frame offsets)

Ethernet: dst[0:6] src[6:12] ethertype[12:14]=`88 19`. Control marker at frame[16:18]:
- `cf ea` op[18:20]=`ff ff` = **cfea announce**. frame[34]=box-width byte (0x08 idle→0x10 for a
  16-in S-1608); frame[37]=**COMMIT byte** (0x00 announce → 0x01 commit = drives box scene-FSM state-4).
- `cd ea` op[18:20]=`01 00` = **op-0100** handshake (probe). Sub-state 0x02 hunting / 0x03
  established. Some blocks carry ASCII "SCEN"/"SYSP".
- `cd ea` op=`01 03`, subtype=frame[20:22]: `00 19`=**chanmap**, `00 0d`=**ENROLL** (reac-pw-only;
  causes the +8), `00 10`=config.
- `cd ea` op=`04 03` = **head-amp record**: frame[34:36]=`01 01` tag, frame[36]=CH,
  frame[37]=param (0=phantom,1=pad,2=sens), frame[38]=value.
- `cd ea` op=`01 01`=SUB01, `01 02`=SUB02 (the REACPW_EST_COMMIT pair; M-200 never sends these).

BOX upstream frames (src `...c4:80:41`) are a different 628-byte layout with control multiplexed
at the SAME offsets (d[16:18]=cd ea, d[18:20]=op, d[20:22]=sub). Key box frames: JOIN=`cd ea 04 03`
(op_len 0x13/0x14); heartbeat=`cd ea 01 03` sub `00 01`; state declare=`cd ea 01 03` sub `00 10`.

## Python pcap parser template (stream, never read whole file)

```python
import struct
def frames(path, maxn=10**9):
    f=open(path,'rb'); f.read(24); n=0
    while n<maxn:
        rh=f.read(16)
        if len(rh)<16: break
        _,_,caplen,_=struct.unpack('<IIII',rh); fr=f.read(caplen)
        if len(fr)<caplen: break
        n+=1; yield fr
# head-amp record: fr[18:20]==b'\x04\x03' and fr[34:36]==b'\x01\x01' -> CH=fr[36] param=fr[37] val=fr[38]
# committing cfea: fr[16:18]==b'\xcf\xea' and fr[37]==0x01
```
Head-amp captures are HUGE (4000 fps × ~1 KB). Slice first: `tcpdump -r big.pcap -w slice.pcap -c 250000`.

## The build → launch → test loop

```bash
cd ~/Devel/audio/reac-pw
ninja -C build && sudo -n setcap cap_net_raw,cap_sys_nice+ep build/reac-pw   # file caps survive relaunch
pkill -x reac-pw            # ALWAYS -x (exact name). NEVER pkill -f 'build/reac-pw' — matches the
                            # launching shell's own cmdline and self-kills it (exit 144).
sudo -n pkill -x tcpdump    # same: -x, never -f '...pcap' (self-kill)
# launch (self-churn test; box re-cold-connects on its own):
HA=""; for ch in $(seq 32 47); do HA="$HA --headamp ${ch}:phantom:1"; done   # CH 0x20..0x2f = all 16 at base 0x20
REACPW_NO_ENROLL=1 REACPW_OP0100_BURST=2000 REACPW_EST_COMMIT=1 nohup build/reac-pw \
  --live enp128s20f0u6 --role master --mixer m200 --tx enp128s20f0u6 \
  --box s1608:S-1608 --src-mac 00:40:ab:c9:cc:04 $HA > /tmp/reac-pw.log 2>&1 &
sleep 35
# STABILITY = the success metric (does the box stop cycling?):
grep -c 'GRANTING -> ESTABLISHED' /tmp/reac-pw.log   # re-establishments
grep -c 'ESTABLISHED -> PROBING'  /tmp/reac-pw.log   # peer-gone drops  (want 0 over 30s+)
grep -c 'box JOIN seen.*-> ESTABLISHED' /tmp/reac-pw.log  # box re-requesting (want 0)
```
Background long-running tcpdump/reac-pw with the Bash tool's `run_in_background`, not `&` inside a
foreground command (it dies on return). Foreground `sleep` up to ~40 s is fine.

## Gotchas

- `pkill -f '<pattern that is also in the pkill command line>'` self-kills the shell → exit 144.
  Use `pkill -x <exename>`.
- Scratchpad tmpfs fills fast with multi-GB captures — `rm` old pcaps between runs; keep slices.
- The box streams AUDIO continuously even while its CONTROL heartbeat stops — a peer-gone drop
  is about the missing control heartbeat, not audio silence. Do not confuse "box present (audio)"
  with "box recognizing the master (control)".
- To capture the M-200 groundtruth: kill reac-pw, plug the M-200, confirm `cc:03` on the wire and
  `cc:04` absent, start tcpdump, power-cycle the box for a clean cold-connect from frame 1.

## Where the knowledge lives

- Running state + head-amp RE status: the operator's memory file
  `reference_reac_headamp_s1608_1to8_confirmed_base20_target.md` (read it first).
- Firmware truth: `~/Devel/audio/reac-firmware-re/devices/S-1608/decompile/S-1608_alldecomp.c`
  (scene-FSM state var `*DAT_0c003c08`, state 4 = commit `FUN_0c003c8a`, master recognition
  `FUN_0c003548` = needs sub-state 0x03 + console byte + width 0x10).
- reac-pw control plane: `src/reac_master.c` (establishment FSM), `src/reac_grant.c` (grant sweep +
  head-amp group-A, all 48 records), `src/reac_ctrl.c` (frame build + box-frame classify),
  `src/reac_pacer.c` (RX ingest + per-slot TX).

## Publishing boundary

Audio repos: author `Pau Aliagas <linuxnow@gmail.com>`, GPG `A14B3E1E1F69EBF4`, no AI trace,
branch + PR (never main). Diagnostic env knobs stay default-OFF (byte-identical when unset) until
a fix is rig-proven. Do not commit RE code without the operator's publish go.
