# The knock: how a silent wire is looked at

A wire nobody pinned, on which no REAC frame is heard for 500 ms, has no master on it. A cold
stagebox on it says nothing until a master speaks. Until reac-pw#40 the daemon answered that
silence by taking the wire as master: the whole stream, 4000 or 8000 frames a second, for as long
as it ran. On the untagged native VLAN of a trunk that is about 96 Mbit/s of broadcast, and a
managed switch shut the port. The rule since then: a master never floods a wire searching for a
box. This page is what the daemon sends instead, what that rests on, and what is not yet known.

Every libreac citation is to the v1.8.0 tag.

## What a real box was seen to answer

| fact | source |
|---|---|
| A cold box does NOT answer a lone announce: one announce every 2 s, tx +2 frames per 6 s, rx 0 for over a minute (S-1608, 2026-09-08) | libreac `include/reac/reac_knock.h:21-25` |
| A cold box answers a master that is DRIVING, "the continuous probing stream a real desk puts on the wire"; both rig boxes (S-0808, S-1608) came up within two seconds of link | `reac_knock.h:25-27`, `:6-8` |
| An S-0808 declared itself 0.700 s after a driving master entered PROBING, about 17 ms after the first transfer's final chunk | `logs/master-default-20260826.log:6,8-12` |
| An S-1608 already talking unicast to a previous master lost presence, flooded broadcast FILLER and declared itself 5.15 s after PROBING | `logs/master-s1608-20260826.log:10,13-17` |
| A hunting master's every non-control slot is an audio FILLER | libreac `src/reac_master.c:1266,1297` |
| A real desk's capture is FILLER almost throughout: 269 701 of 270 593 frames | libreac `tests/corpus-baseline.txt:54` |
| A real M-200 probing with no box: 347 control frames per cycle beside its FILLER | `tests/corpus-baseline.txt:8` |
| A real M-200i repeats the whole transfer (header, 341 chunks, final, 0.684 s) every 2.695 s inside its running stream; the box declared itself at +17.8 s | reac-protocol `spec/reac.ksy:218-231` |
| The box's commit report comes 8.4 ms after the final chunk; ENROLL +209.6 ms; the box's `04 03` burst 1.489 s after that | libreac `tests/test_master_capture.c:14-21` |
| A box's receiver recovers word clock from frame arrival cadence, one slot per frame | reac-protocol `wire-format.md:307-311` |
| A cold box floods broadcast FILLER on PHY-up, bounded (5459 frames over ~1.36 s at 48 kHz), then goes unicast-only and cold-connects only to a master it heard | libreac `include/reac/reac_fsm.h:15-22,43-49` |
| A box that DROPPED (host suspend) ignored ~1620 whole transfers; it leaves that state on a PHY link-up and on nothing else | `src/reac_wake.h:6-18` |
| libreac's `fake_box` reacts only to scene-transfer and group-map frames: FILLER is ignored by construction | libreac `tools/fake_box.c:228-254` |

**What the rig showed, 2026-10-27.** Until that day no capture, rig note or test showed a real
box answering the control frames with the FILLER slots left empty; every recorded answer had come
inside a continuous stream. The rig then answered it 75 times (below): both box families answer
the control knock, within a second.

## The knock

A knock is one hunting cycle of libreac's own master, stepped slot by slot at the wire's cadence
from the transfer's header, as the pacer steps it, with ONE content: the frames the master stamps
with a control block go out (the scene transfer: header, 341 chunks, final; the channel map; the
announces), and every FILLER slot is left empty. 347 frames per knock at 48 kHz and at 96 kHz,
341 of them in a 0.68 s burst.

There is no stream knock and no plan to choose one (reac-pw#61). Until 1.0.40 a `knock =` key
could select `escalate` (two control knocks, then one cycle of the whole stream) or `stream`;
1.0.40 made `control` the default and 1.0.41 removed the other two and the key: a plan that sent
one cycle of the stream onto a wire nothing had answered on was a flood by configuration (45 199
untagged frames in 95 s onto a switch's native VLAN, below), and the operator's law is that no
segment is ever streamed at before a box answered on it, in any case. A `knock =` line in the
conf is now refused by name, with the reason. The stream starts on the answer.

The knock is inside these bounds:

- **The backoff.** Knock n+1 opens one cycle after knock n, doubled for every knock nothing
  answered, up to 30 s (`REAC_KNOCK_WIRE_PERIOD_MAX_NS`): 2.695, 5.389, 10.778, 21.556, 30, 30 ...
  The first frame heard after 500 ms of silence, or a fresh link, starts the period again, and
  a knock that was resting opens at once.
- **The first evidence ends it.** A box's declaration, its JOIN, a desk: the knock stops and the
  master stream starts on that box, with its answer handed over. A box's presence flood (FILLER of
  unknown role) does not end it: that box is waiting to hear a master.
- **The host's NIC gets no edge** (reac-pw#46,
  2026-10-26-a-host-nic-is-never-bounced-or-streamed-at). An interface that carries an address of
  a scope wider than link, a netdev stacked on it (a VLAN sub-interface, ours or the host's), or a
  default route is the host's: the desk reaches its switch and its console through it, and a box
  behind it is reached through its VLAN, never by cycling the parent. The knock on it is the same
  control knock as on any wire, and the knock's wake ladder refuses the PHY edge by name
  (`HOST_NIC`). A bare NIC, the direct-cable case, is bounced. The fact is read from rtnetlink
  once a second; when it cannot be read the NIC is treated as the host's.

## What a wire with nothing on it carries

Measured on the wire model (`tests/test_reac_knock_wire.c`, arm A, 96 kHz):

| | |
|---|---|
| knocks opened in 600 s | 23, at gaps of 2.695, 5.389, 10.778, 21.556 s, then 30 s |
| frames in 600 s | 7 981, all control frames, 0 FILLER |
| at the ceiling | 347 frames per 30 s, about 12 frames a second, about 138 kbit/s |
| inside a knock | 500 chunks a second for 0.68 s, about 6 Mbit/s |
| the old probe, for scale | 8000 frames a second, about 96 Mbit/s at 96 kHz, for ever |

A switch with broadcast storm control at 1% of 100 Mbit/s (1 Mbit/s) sees the chunk burst above
its threshold for the 0.68 s it lasts; the backoff lowers the average, not the burst. On a bare
NIC the knock's wake ladder may make up to 2 PHY edges on a wire nothing answers
(`REAC_WAKE_MAX_BOUNCES`).

On the host's NIC (an address beyond link scope, a stacked netdev, a default route) the knock is
the same and there is no edge at all; the wire model's arm H measures the same 23 knocks and
7 981 frames in 600 s, 0 FILLER. The desk's trunk parent, which carries the management address
and every REAC VLAN, is this case.

## A master whose box leaves

`src/reac_empty_master.h`: an unpinned master stops streaming at once when libreac's master
gives an enrolled box up (its own ride-through of about 6.5 s, `reac_master.h:141-149`, as a live
M-200i does), and 2 s after the last box frame while it is still courting one. The wire then goes
back to listening and knocking. The first cut waited 10 s on top of a 5 s latch.

## The rig check: the condition for merging reac-pw#41

The wire model proves the knock's content, its bounds and the backoff. It cannot say whether a
real box answers that content. This check does, once per box family the rig has. Families, by libreac's box table
(`src/reac_ctrlblk.c:413-654`): the compact S-series (S-0808, S-0816, S-1608, S-2416), the
S-4000 modular units (S-4000S-3208, S-4000S-0832, S-4000D, S-4000M), and a FreeREAC box (another
reac-pw with `role = box`).

Set up, per family: the branch's daemon on a host with one free NIC, call it `X`; the box on a
direct cable to `X`, no switch; no conf file except the one each step writes, as
`~/.config/reac-pw/reac-pw.conf.d/99-rigcheck.conf`. "The roster" is the `reac-pw` node's
properties (`pw-dump`, find `reac.roster = 1`), read for the row whose `.name` is `X`.

1. **Cold.** Power the box and wait 60 s with no daemon running (its PHY-up flood is over;
   this is the case that needs a knock). Start the daemon, no conf, watch 120 s.
   - PASS: the row goes `knocking` -> `probing` -> `established`, and the journal says
     `ANSWERED: ... during knock N`. Record N and the time.
   - FAIL: the row is still `knocking` at 120 s and its status shows at least 6 knocks.
     Record it: this family does not answer the control knock, and there is no other knock to
     offer it (the "Not verified" list, below).
2. **Cold, again.** Restart the daemon with the box powered and silent as in 1.
   - PASS: `established` within 60 s; record the journal's `ANSWERED` line (knock 1 or 2).
   - FAIL: still `knocking` at 60 s. Do not merge.
3. **Default, the box powers up while the daemon runs.** PASS: `established` within 30 s.
4. **The box leaves.** Put an unmanaged switch between `X` and the box (pulling a direct cable
   drops the link, and a wire with no link is not knocked on). With the box `established`, power
   it off. PASS: within 10 s the journal says the master `stops streaming at nobody`, the row is
   `knocking`, and `ip -s link show X` read 10 s apart after that line shows fewer than 1 100 TX
   packets (at most three control knocks).
5. **Nothing on the wire, ten minutes.** `X` linked to a switch port with nothing else on it.
   Read `ip -s link show X` TX packets at start and after 600 s. PASS: the difference equals the
   journal's knock frames to within 1% and is at most 25 x 347 per link-up the journal reports,
   at either rate.
6. **Behind a trunk** (once, any family). `X` is a trunk; the box on an access port in VLAN v.
   (a) Power the box after the daemon starts: PASS when the roster shows `X` as `trunk` and
   `X.v` `established`, with no conf. (b) Box powered first, then the daemon, with `X.v` made in
   the host's network config: PASS when `X.v` is `established`.

**Merge when** steps 1 to 6 pass for every family on the rig, and record step 1 per family in
the PR. A family that fails step 1 is recorded under "Not verified" as a box the knock does not
find; it is not an argument for a stream knock (reac-pw#61).

### What the rig answered (2026-10-27, reac-pw#58, 1.0.40)

Step 1 passed for both families on the rig: an S-1608 (compact S-series) and an S-4000S-3212 (S-4000
modular) each answered a **control knock** within a second of it, on a direct NIC and again on a
VLAN sub-interface of a trunk. Over the day's takes (the 1.0.39 cycles included, where every
enrolment was lost and knocked again) the journals hold 75 answers: 65 to knock 1, 9 to knock 2, 1 to
knock 3, every one of them a control knock and none a stream knock.
So `control` became the default in 1.0.40, and 1.0.41 removed the other plans (reac-pw#61). What
forced the change: with the old `escalate` default, the daemon streamed a wire it had not yet
heard anything on, and a trunk parent is exactly such a wire until the first tagged frame
arrives. On the rig that was 45 199 untagged REAC frames in 95 s onto the switch's native VLAN,
waking nothing (the boxes sat behind access ports in VLANs 11 and 12). The control knock reaches
the same parent with 347 frames per knock and the backoff above (3 814 frames in 185 s on the
second take, peak 345 a second); the VLANs are found from the switch's own tagged traffic as
before.

## Not verified

- That a box family other than the two on the rig (compact S-series, S-4000 modular) answers a
  control knock. One that does not is not found by the knock; there is no fallback that streams
  an unanswered wire, by law (reac-pw#61), so such a family would need a knock content of its own,
  proven on the rig first.
- Two boxes on two VLANs that were powered before the daemon started: with no tag heard and no
  sub-interface in the host's config, nothing finds them (README, "A box behind a trunk").
- A box that answers only the stream, on any wire: the knock never streams, so it is not found.
  No box on the rig is one.
