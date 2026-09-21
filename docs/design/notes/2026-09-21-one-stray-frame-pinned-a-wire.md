<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# One stray frame pinned a wire for the life of the process

2026-09-21 22:29. A conformance finding against
[`2026-09-16-segments-and-roles-are-autodetected.md`](../specs/2026-09-16-segments-and-roles-are-autodetected.md)
§2 — *"silent → we MASTER and flood"*. Nothing here changes a design. Two EVER-latches in
this daemon let one misattributed frame suspend that rule for ever on one segment, and the
fix is #102's own ruling (*"the hunt asks about the wire as it is NOW"*) applied to the two
places that did not get it.

## 1. What the desk did, twice, ten minutes apart

`journalctl --user -u reac-pw --since "2026-09-21 22:17" --until "2026-09-21 22:28"`. Two
restarts of the same service, same cabling: an S-1608 (`00:40:ab:c4:80:41`) on `enp131s0`
and an S-0808 (`00:40:ab:c4:dc:9c`) on `enp128s20f0u6`, a DIRECT point-to-point USB NIC
with no switch on it.

**22:17:45, the defect.** Both sniffers opened in the same second, and BOTH reported the
same box:

```
S_SEGMENT_HEARD [enp131s0]        REAC heard — box 00:40:ab:c4:80:41 (16 ch)
S_SEGMENT_HEARD [enp128s20f0u6]   REAC heard — box 00:40:ab:c4:80:41 (16 ch)
```

The second line is impossible on the wire: that MAC is the S-1608, and `enp128s20f0u6` is
a cable with one S-0808 on the far end. `enp131s0` went on to `no master heard in 3 s and
a box is present — taking the wire as MASTER` at 22:17:48 and had the S-1608 established at
22:17:52. `enp128s20f0u6` printed **nothing further for nine minutes**: `listening — role
auto`, `linkState: probing`, `masterState: none` on the console's `/reac/segment` row, and
`ip -s link` measuring 0 RX packets in 2 s. The S-0808's eight inputs were off the desk
until a human power-cycled the box.

**22:26:14, the same code, no defect.** `enp131s0` heard the S-1608; `enp128s20f0u6` heard
nothing, and so:

```
[enp128s20f0u6] no REAC heard in 500 ms — ... taking it as MASTER and probing until a box cold-connects
[enp128s20f0u6] MASTER role (M-5000 profile) ... probing until the box's cold-connect (cdea 04 03) arrives
S_SEGMENT_HEARD [enp128s20f0u6] REAC heard — box 00:40:ab:c4:dc:9c S-0808 (8 in / 8 out) (8 ch)
```

The cold box re-enrolled in three seconds with no power-cycle. **The courting already
exists and already works.** The whole defect is the first restart's stray frame.

## 2. The design gate

1. **Which primitive?** `reac_knock` — the daemon's one licence for "this wire carries no
   master, so take it". No new mechanism, no new role, no new configuration.
2. **Extend the vocabulary first?** No. The behaviour the operator asked for (*"we should
   be able to re-enrol the S-0808 as an M-200 does"*) is what §2 already rules and what
   22:26:14 measured. The fix is that two predicates must be about the wire NOW.
3. **Why isn't X a Y?** Why isn't this "remember the box in the roster and court the
   remembered width"? **Because an M-200 remembers nothing, and neither may we.** In
   `reac-captures/m200-enrol-441k-2026-09-13/analysis.md` the M-200 `00:40:ab:c9:cc:03`
   masters across 13.066 s of total box silence (1789327535.553 → 1789327548.620): it
   never stops, it declares `box_in_width` **`0x08` idle** with `box_count 0`
   (1789327542.128116, 1789327543.130245), it runs a scene transfer every 2.6957 s into
   the empty wire, and it widens to `0x10` only when the box's own commit report arrives
   (1789327550.105055 → console `0x10` at 1789327550.313223, grant at 1789327551.820802).
   libreac's `tests/test_master_carriers.c:294` gates exactly that shape — *"the announce
   width tracks the recognized box (0x08 idle, 8/16/32) and the box count rises 0→1 only
   at the grant"*. A master is a master on an empty wire; a remembered width is not a
   precondition and would be a second source of truth beside the box's own declaration
   (`feedback_box_facts_come_from_the_wire`). **The roster is not read, not written and
   not consulted by this change.**

## 3. The two latches, and why only one wire fell over

`hearing_hunt` skips a sniffer whose tap has classified no UNTAGGED frame while the
sniffer has heard something — *the tap is the authority*, and a sighting the tap has not
placed on a VLAN yet is of unknown VLAN. Both halves of that guard were **EVER**
questions:

- `reac_hunt_heard_anything()` — "has any REAC gear been heard on this wire AT ALL";
- `tp->untagged == 0` — "has the tap classified an untagged frame here, ever".

On `enp131s0` the tap kept classifying the S-1608's real frames, so `untagged` went past 0
within a poll and the guard let go. On `enp128s20f0u6` the tap classified **nothing at
all** — the stray frame reached the sniffer socket and not the tap — so `untagged` stayed
0, `heard_anything` stayed 1, and the guard held for the life of the process. That
asymmetry, in one journal second, is what names the guard as the fault.

Behind it sits the second latch: `REAC_KNOCK_CANCELLED` was terminal
(`src/reac_knock.c`), so even with the guard gone the masterless observation could never
re-open on a wire that had been heard once.

**The fix, both halves the same sentence:** a wire that has carried not one REAC frame for
`REAC_KNOCK_LISTEN_NS` has no master on it — *whether or not it once did*. The licence
re-opens from the LAST frame heard, so a wire with a real master (a frame every 125–272 µs,
1837 consecutive slots inside the window) never re-arms, and a wire that was heard for a
moment and went quiet is decided again exactly as on a clean start. The tap-authority wait
is bounded by the same bar `topo_trunk_now` already uses for "is this still true"
(`REAC_HUNT_WINDOW_NS`), in a pure module (`reac_tapwait`) with the inputs written down.

## 4. Where the stray frame came from — a libreac finding, not fixed here

`libreac/src/reac_capture.c:27` opens the sniffer as
`socket(AF_PACKET, SOCK_RAW, htons(ETH_P_REAC))` and only then does `SIOCGIFINDEX` and
`bind()`. A packet socket created WITH a protocol is live on **every interface on the
host** until the bind lands. libreac's own `include/reac/transport/reac_topo.h:29-31` says
this in as many words for the tap socket — *"DEAF UNTIL IT IS BOUND — the protocol is
given to bind() and not to socket(), because a packet socket created with a protocol hears
every interface on the host until the bind lands (#18)"*. The capture socket never got that
fix. With an S-1608 mastering `enp131s0` at 8000 fps, a frame lands in that window about as
often as the window is open, which is why 22:17 leaked and 22:26 did not. Filed against
libreac; this daemon's fix stands on its own, because a daemon that can be pinned for ever
by one misattributed frame is a defect whatever produced the frame.

## 5. Considered and refused: "a box has one wire"

Ignoring a sighting whose source MAC is already ESTABLISHED on another segment would not
have caught this. Both S_SEGMENT_HEARD lines are stamped 22:17:45; the S-1608 only reached
ESTABLISHED on `enp131s0` at 22:17:52, seven seconds later. At the moment of the stray
frame there was nothing to compare against, and a rule that refuses a box which has
legitimately been re-cabled would cost more than it saves.
