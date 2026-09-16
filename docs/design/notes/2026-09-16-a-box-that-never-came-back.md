# The box that never came back — msi, 2026-09-16

Raw evidence for the ruling in
`docs/design/specs/2026-09-16-a-dropped-box-wakes-on-a-phy-edge.md`. Journal lines are
verbatim from the desk's own `journalctl --user -u reac-pw`; the counter deltas were read
from `/sys/class/net/enp131s0/statistics/` with a positive control in the same command.

## What happened

reac-pw 1.0.10, MASTER on `enp131s0` (untagged), 96 kHz / 8000 fps, ETF backend. An
S-1608 in slave mode, `00:40:ab:c4:80:41`, enrolled. The desk went to s2idle at ~13:30 and
resumed at 14:38. `CLOCK_MONOTONIC` shows the gap: the daemon's own clock advances 152 s
across 77 minutes of wall time.

    13:20:25 reac-master: [117023.558462] box JOIN seen (cdea 04 03, unicast from 00:40:ab:c4:80:41) -> ESTABLISHED
    13:20:31 reac-master: [117029.660097] box JOIN seen (cdea 04 03, unicast from 00:40:ab:c4:80:41) -> ESTABLISHED
    13:20:49 reac-master: [117048.165234] box JOIN seen (cdea 04 03, unicast from 00:40:ab:c4:80:41) -> ESTABLISHED
    ... the box re-JOINed on a ~2 s grid for the whole pre-suspend hour ...

    14:38:52 reac-pw: [enp131s0] unpinned — listening for REAC
    14:38:56 reac-pw: [enp131s0] no REAC heard in 500 ms — a master fills every slot, so this wire has none: taking it as MASTER and probing until a box cold-connects
    14:38:56 reac-pw: [enp131s0] no master heard in 3 s and a box is present — taking the wire as MASTER: probe, grant, establish
    14:38:56 reac-master: [117245.541790] IDLE -> PROBING (timer)
    14:39:06 reac-master: [117255.541517] still PROBING: rx_box_frames=0 rx_joins=0 (carrier is up. A box that is LINKED AND SILENT looks like this ... so do not bounce it yet ...)

That line then repeated every 10 s for 33 minutes, through a full service restart, and for
39 minutes after it:

    15:11:48 reac-master: shutdown in PROBING — tx=15778180 err=107 late=18123 | rx_box_frames=0 rx_box_ctrl=0 rx_joins=0 grant_attempts=0 | drops: peer-gone=0 bye=0 mac-change=0 grant-timeout=0
    15:11:52 systemd: Started reac-pw.service
    15:11:56 reac-master: [119225.614211] IDLE -> PROBING (timer)
    15:48:26 reac-master: [121415.614072] still PROBING: rx_box_frames=0 rx_joins=0 (carrier is up. ...)

**73 minutes, two processes, ~1620 completed scene pushes** (the PROBING cadence completes
one transfer per `cycle_len` = 21556 slots = 2.6945 s at 8000 fps) and not one frame back.

## The wire, measured, with a control

At 15:56, `/sys/class/net/enp131s0/statistics/` over a 10 s window:

    tx_packets +80028   (8003 fps — the master is flooding, exactly as it should)
    rx_packets +0       (in the sample taken minutes earlier: +0 over 5 s)
    lo rx_packets +1184 over 1 s — the control, in the same command, proving the reader works

So this is not a receive-path fault and not a pacing fault: the daemon transmits at the
full cadence and the far end answers nothing at all. The health line agrees that the
transmit side is healthy — `launch-miss 0.00/s (qdisc drops 127)`, `re-base 0.00/s`,
`dropped_slots=0` — 127 qdisc drops against 15.8 million frames.

## What ended it — and what it proves

Nothing we sent. At **15:51:12** a DIFFERENT box appeared on the same wire, a freshly
powered S-4000S `00:40:ab:c4:06:80`, and enrolled in under a second:

    15:51:12 reac-pw: [enp131s0] REAC heard — box 00:40:ab:c4:06:80 S-4000S (32 in / 8 out) (8 ch): this interface is a segment
    15:51:12 reac-master: [121581.300328] PROBING -> GRANTING (rx CONFIG)
    15:51:19 reac-master: [121588.465443] GRANTING -> ESTABLISHED (timer)

A box that had just been given a PHY link-up answered our push in milliseconds on the same
wire, with the same master, at the same instant the S-1608 had been ignoring it for 73
minutes. The one difference between the two boxes at that moment is the PHY edge.

The rig moved on after 15:51 (the main session owns it); the window above is fixed.
