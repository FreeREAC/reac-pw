<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# Auto role per segment — what the daemon may detect about who masters a wire, and what it adopts

Status: RULED by the operator 2026-09-16 ("we should autodetect master/slave/SP mode and adapt to
it"); normative for `reac_hunt`'s callers in this daemon and for the console that writes its conf.
**§4 is SUPERSEDED, same day, by
[`2026-09-16-segments-and-roles-are-autodetected.md`](2026-09-16-segments-and-roles-are-autodetected.md)**:
`REAC_ROLE_<segment>` is retired as a role source and as a segment declaration, the console writes
no role key at all, and the one override is `reac-pw.conf`. §0, §1, §2, §3 and §5 stand.
Companion to openmixer's `2026-08-20-reac-master-arbitration.md` §8 (the intent/observation law),
`2026-09-13-reac-plug-and-play.md` §4 (the role vocabulary, option C) and this repo's DESIGN.md.

## 0. The live case this answers

2026-09-16, plain PCI NIC `enp131s0`, an S-1608 with its REAC Mode switch on **M**. The console's
generated `~/.config/reac-pw/reac-pw.env` carried `REAC_ROLE_enp131s0=master`. The daemon refused
the segment (`rival-master-box`, the box `00:40:ab:c4:80:41` mastering at 16 ch), published a
door-only node, and moved no audio. The operator saw "not detected".

**The daemon was right and the file was wrong.** A wire pinned `master` with a box already
mastering it is the one case `reac_hunt` refuses, and it refuses it on purpose (2026-09-09): the
console never fights for a wire it forced itself onto, and the remedy is at the box's own switch.
The defect is that nothing asked for that pin tonight — a stored intent from a bring-up session
became a launch-time decision taken with no observation to make it against.

## 1. THE FOUR CASES, and which of them the frames can actually decide

A segment in `auto` decides from GEOMETRY and CAPTURED control signatures only — never from a role
byte, never from a signature nobody has captured (arbitration §4).

| # | the wire | detected by | adopted | proven |
|---|---|---|---|---|
| a | nobody masters | `reac_knock` silence licence (no frame for the listening window), or a box heard with no master for `REAC_HUNT_WINDOW_NS` | we master | `tests/test_reac_hunt.c`, `tests/test_reac_enrolment_binding.c` (the licence itself is libreac's since 1.4.0 — `libreac/tests/test_reac_knock.c`) |
| b | a DESK masters | a foreign master whose frames carry the 40-channel downstream geometry (`REAC_RIVAL_DESK`) | defer — `auto` TAPS (courtship option C, 2026-09-14); an explicit `recorder` slaves | `tests/test_reac_hunt_captures.c` arm `desk`, real M-200 bytes |
| c | a BOX in M masters | a foreign master whose frames carry a BOX width (`REAC_RIVAL_BOX`, `rival_channels` = the box's own width) | slave-join it; the segment is sized from `rival_channels` and the capture node carries the box's identity (`reac.box.mac`, DESIGN.md 0.5.2) | `tests/test_reac_hunt_captures.c` arms `box-master-auto` / `box-master-pinned`, real S-1608-on-M bytes |
| d | a box in SP | **NOT DETECTABLE TODAY — see §3** | nothing; say so | §3 |

`rival_channels` is the evidence the verdict was made from and is never re-derived by the caller
(`reac_arbitration.h`). A box on M offers **no head-amp**: it is a preconfigured box with no mixer
behind it, by design, so the console's head-amp cells for that box are dead and must say why.

## 2. What a refusal still owes

A refused segment is PUBLISHED as a door-only node carrying the refusal props (DESIGN.md 0.5.1) —
a refusal nobody can see is indistinguishable from a daemon that is not running. `auto` refuses
only case (d)-shaped evidence: a foreign master whose geometry does not read.

## 3. SP (split) mode — what the wire shows, and it is nothing

The split device's own frame is `SPLIT_ANNOUNCE` (`0xceea` at the control type word), which
libreac parses as `REAC_CTRL_SPLIT_ANNOUNCE` and **deliberately classifies as role UNKNOWN**:
never captured. The corpus audit of 2026-09-13
(`reac-captures/analysis/2026-09-13-announce-bytes-and-headamp-base.md` §4b) counted **zero**
`0xceea` and zero `0xc2ea` at the type word across 6 450 414 REAC frames in 111 files, against
17 040 `0xcfea` found by the same scan — a positive control in the same pass. The five-step split
handshake is source-derived (a GPL macOS driver by way of reac-aes67), not measured.

Two consequences, and they are rulings:

1. **No SP classifier ships on zero frames.** A frame kind nobody has captured must not flip a
   segment's topology (arbitration §4). The daemon's conservatism already covers it: a
   `SPLIT_ANNOUNCE` is a visible sighting with no role, so it cannot make us slave, master or
   refuse by itself. `tests/test_reac_hunt_captures.c` pins that: a synthetic `0xceea` frame
   changes no verdict.
2. **What SP looks like from our side is already two known cases.** The mode switch is read at
   boot and never re-read; SP splits the box's I/O across two REAC ports so two consoles share one
   stagebox, and **M is the splitter's clock role** — clock-slave on the uplink, master on the
   split outputs (`reac-protocol` wire-format.md:306-330). So on the UPLINK segment an SP box is a
   slave box (silent until mastered, case a); on a SPLIT port it presents exactly as case (c). The
   2026-09-10 trial against a plain master — box silent, sync lamp on — is consistent with both
   and distinguishes neither.

Owed, and named rather than guessed: `sp-unsupported` as a segment refusal code, to be minted in
libreac beside `rival-master-{box,unknown}` **when, and only when, a capture of a real split device
exists**. The rig that would take it is in
`reac-captures/analysis/2026-09-13-announce-bytes-and-headamp-base.md` §4c.

## 4. What the console must write (the other half of the fix)

The per-segment key `REAC_ROLE_<segment>` is read at launch, when there is no observation to
decide against. `master` is the one value that can collide with a master already holding the wire.
So a console projects `auto` for an intent of `mixer` and lets the hunt decide, then asserts the
intent live over `reac.cfg.role` once the segment has announced. Stated and gated on the console
side in openmixer's `2026-08-20-reac-master-arbitration.md`, ninth amendment (2026-09-16).

Writing `auto` still DECLARES the segment: `reac_declared_vlan.h` reads the KEY's name, never its
value, so a declared VLAN is still minted at start.

## 5. Amendment, same day — two operator rulings

### 5a. RULED: "We set the daemons to enroll any box, master or slave."

§1's case (c) becomes the ONLY outcome for a box, whatever pinned the segment. The one exception
left by 2026-09-09 — a wire the operator pinned `master` with a box mastering it is REFUSED, and
the remedy is the box's own switch — is retired: the refusal cost the operator that box's entire
audio to make a point about a pin, and a box on M is a clock like any other.

**`rival-master-box` is retired as an OUTCOME.** What survives is the INFORMATION: the box offers
no head-amp (§1), and the console says so as a warning rather than as a refusal.

*The owed change, named exactly.* libreac `src/reac_hunt.c`, `decide()`'s pinned arm:

```c
if (h->pin == REAC_ROLE_MASTER && h->arb.state == REAC_SEGMENT_FOREIGN &&
    h->arb.rival == REAC_RIVAL_BOX)
        return REAC_HUNT_REFUSED;
```

becomes `REAC_HUNT_SLAVE`, and `reac_hunt.h`'s contract paragraph ("what survives is the
contradiction: a wire the operator pinned MASTER with a box mastering it is refused") goes with
it. `REAC_HUNT_REFUSED` stays for the ONE case that still earns it: a rival whose geometry does
not read (§4's conservatism — there is nothing to size a segment from). Not done in this lane:
libreac is a third repo with no worktree in it, and the change is one branch plus the
`test_reac_hunt.c` arm that pins the old answer today
(`tests/test_reac_hunt_captures.c`'s `box-master-pinned` arm pins it against REAL bytes and is the
test that must flip with it).

The CONSOLE half is done: `decideReacSegmentRole` joins a box under every intent, and the
head-amp fact is raised as `stagebox:master-mode-no-headamp` (openmixer commits of 2026-09-16).

### 5b. RULED: "The daemon has been running since 01:04 without picking up the new env setting, since that only applies on restart."

A role or rate change on `/reac/segment/{name}` must APPLY LIVE. Measured the same night: the
console regenerated `reac-pw.env` correctly and the running daemon carried on in the role it
launched with, because `REAC_ROLE_<segment>` is read once at start (`reac_conf`), by design.

**The rule.** The daemon RE-READS its conf on a signal and RE-ELECTS the named segment only:

- **SIGHUP re-reads every layer `reac_conf` reads** and, for each segment whose resolved
  `REAC_ROLE_<segment>` (or `REAC_RATE_<segment>`) has CHANGED, performs that segment's own
  re-election — the existing role swap (`reac_role_swap`, a clean listener close + open in the
  other engine) and the existing `reac_pacer_apply_rate`. A segment whose resolved value is
  unchanged is not touched at all.
- **NO OTHER SEGMENT MOVES.** This is the ruling's own load-bearing clause: the S-0808 on the USB
  link must not drop because `enp131s0`'s role changed. One daemon, N listeners (auto-spine §5) —
  the re-election is per listener, and the others do not observe it.
- **The console sends the signal from the row's PATCH**, not from a restart and not from a unit
  file: the row writes the env (it already does) and then signals. A PATCH that changed nothing
  signals nothing.
- **The row reads back what is SERVED**, by query-and-compare against `reac.role` — the end the
  daemon is RUNNING, published by whichever engine opened. NOT `reac.cfg.role`, which is the
  console's SPA_PARAM_Props write channel and which the daemon publishes nowhere: measured on the
  live graph 2026-09-16, every `reac-playback.*` door carried `reac.role` and
  `reac.cfg.role.state` and not one carried `reac.cfg.role`. openmixer's console read the wrong
  key until that night; it is fixed there, and `reac.role` is OWED into libreac's `reac_cfg.h`
  (it lives only in reac-pw's `reac_role_cfg.h` today, which is why the console still spells it
  as a literal).

Not built in this lane. It is a main-loop signalfd handler plus a per-listener re-election path,
and it cannot be proven without the operator's rig: the honest proof is a role PATCH on one
segment with a second segment's box audible throughout.

### 5c. Open, and in scope for the next pass: an S-1608 in S mode that answers nothing

Live, 2026-09-16: with the box switched to **S** on plain `enp131s0` and us mastering, the box
sent **0 frames** (link 100 Mb full, partner negotiating). A cold stagebox is silent until a
master announces TO it, so silence is the expected STARTING state — but it must end at our first
announce, and it did not. Power-cycle, cable and port checks sit with the operator.

If the box is healthy, the difference is in what our master emits: compare reac-pw's first frames
on a cold wire against a desk's, frame for frame, from
`reac-captures/captures/m200i-s0808-48k-mirror__m200-BIDIR-coldboot-2026-07-11.pcap` (an M-200
cold-booting a box: 302 master announces, 21 grants, 138 box heartbeats, the whole enrolment) and
`m200i-none-48k-clean__m200-s1608-realbox-establish-2026-07-11.pcap`. `tests/reac_conformance_golden.inc`
already pins our announce bytes against captured desk frames, so a byte difference in the
ANNOUNCE would be caught there; what is NOT pinned is the SEQUENCE and cadence of the cold-connect
opening, which is where a frame the desk sends and we do not would hide.

### 5d. MEASURED 2026-09-16 — the wire is FULL: four masters on a 100 Mbit/s port, 75% discarded

The S-1608-in-S silence is not a frame-content question. `enp131s0` is **100 Mbit/s full
duplex** and carried **four declared 96 kHz master segments** — the untagged one plus VLANs 11,
12 and 13, and three of those four have no box on them at all (`reac.box.mac: none`).

| measured | value |
|---|---|
| offered by four masters (8 000 pps x 1 516 B each) | **387 Mbit/s** |
| on the wire (`tcpdump`, 5.83 s, 0 dropped by the kernel) | 8 229 pkt/s = **99.6 Mbit/s** |
| `tc -s qdisc` on the port, per second | sent 8 238, **dropped 23 784, overlimits 23 783** |
| per-VLAN etf qdiscs (`enp131s0.11`) | 106 dropped LIFETIME — the parent's queue is where it goes |
| what reached the wire, per segment | untagged 1 927/s, vlan11 2 084/s, vlan12 1 607/s, vlan13 2 451/s |

**It is not a family of frames, it is three quarters of ALL of them, spread evenly.** Audio and
invitations alike: every segment's master downstream reached the wire at ~25% of the 8 000 pps
the protocol needs, so no box on that port could ever lock to it, and `cfea` invitations were
thinned in the same proportion. That is exactly rx = 0, at 96 kHz and at 48 kHz alike — four
segments at 48 k still offer 194 Mbit/s.

**Every sign of health stayed green**: four nodes up, each pacer counting its own `tx=812 000`,
and the discards on a qdisc counter nobody reads. This is the silent clamp — delivering a
quarter of a stream and reporting success.

**THE FIX IS AT THE SOURCE, and it is a refusal.** `src/reac_link_budget.{h,c}`: a REAC master
costs `pps x (1492 + 24) x 8` bit/s — 97 024 kbit/s at 96 kHz — and `listener_open` refuses to
open a master TX path that does not fit in its PHYSICAL port's rate beside the masters already
running on it (a VLAN's budget is its parent's; the load is counted from open engines, never
from the roster). The budget is the link itself with no headroom fraction, deliberately: one
96 kHz master is 97% of a 100 Mbit/s port BY DESIGN, and reserving anything would refuse the
only configuration this protocol has at its top rate. What it refuses is the SECOND stream.
A link speed that cannot be read (a veth, a down port, a netns) is "unknown", never "full".

*What this will do on the live rig, said before it is deployed:* admission is FIRST COME. On
`enp131s0` exactly one of the four segments will open and the other three will refuse, loudly,
naming the port and the committed load. Which one wins is the order they open in — so the
operator's real fix is to stop declaring three master VLANs on a port that has one box, and the
refusal is what tells them to.

*And the announce-cadence finding this section used to carry is withdrawn.* The 2.0 s median
gap measured on the wire was this drop, not a cadence: a rate measured through a 75% discard is
a measurement of the discard. Re-measure the announce cadence against a desk AFTER the budget
refusal lands, and tighten it only if it is still slower than the M-200's clustered ~2/s (302
announces in 149 s, never a gap over 1.003 s).

## Amendment 2026-09-20 — the link budget is held by what CARRIES something, and an empty prober yields it

**The live case, reac-pw#107**, home rig, reac-pw 1.0.21 / libreac 1.3.0. `enp131s0` links at
**100 Mbit/s** and carries the show rig's VLAN sub-interfaces `.11`/`.12`/`.13`; the S-1608
(`00:40:ab:c4:80:41`) sits on the UNTAGGED parent. Every segment starts `auto`, every one of them
takes the wire as MASTER after §5d's 500 ms silence licence, and `enp131s0.11` — an **empty VLAN,
nothing on it, ever** — won that race. One 96 kHz master commits 97 024 of the port's 100 000
kbit/s, so §5d's admission then refused every later segment on the same port, including the only
one with a box on it:

```
[enp131s0.13] heard, but the segment did not come up — sniffing again in 5 s
         97024 kbit/s of enp131s0's 100 Mbit/s is already committed to other REAC masters
```

690 of those lines in 30 minutes; the box never enrolled. **Proof that it is the ORDERING and not
the admission:** `[segment enp131s0.11] ignore = yes` (and `.12`) in `reac-pw.conf.d`, restart, and
the parent took the port — `S_SEGMENT_HEARD [enp131s0] … S-1608 (16 in / 8 out)`, enrolled in 3 s.
Nothing else changed.

**§5d's admission stands, unamended.** Three masters on a 100 Mbit port lost 75% of every segment's
frames; a quarter of a stream is a silent master with every health sign green, and refusing is the
only honest answer. What §5d never said is WHO may hold the budget, and first-come was its stated
(and, that night, wrong) answer: a segment PROBING with no box has nothing to carry, and it was
keeping the port from a segment that had just heard one.

### The rule

**A segment that HEARS REAC GEAR takes the port's budget from holders that have heard NOTHING
and are PROBING WITH NO RECOGNISED BOX. They yield; nothing else ever does.**

**The two sides of that sentence are deliberately NOT symmetric, and the asymmetry was measured
before it was written.** The issue names the trigger `S_SEGMENT_HEARD`, which is any REAC sighting;
it names the holder's disqualifier as a RECOGNISED box. A cold stagebox cannot supply the second
until somebody masters it: its presence flood is broadcast FILLER, which `reac_disco`'s direction
discipline classifies role-UNKNOWN by construction ("a box's presence-flood and a master's
downstream audio are byte-identical in kind"), and its config-announce — the frame that makes it a
`box` with a model — only arrives once a master is driving the wire. Measured on the veth,
2026-09-20: the segment with the box read `S_SEGMENT_HEARD … unknown 00:40:ab:61:23:95 (16 ch)` and
never upgraded, for as long as the budget kept its master from starting; the same box on a segment
that HAD the budget went `unknown` → `box` → `box S-1608 (16 in / 8 out)` in seconds. **Requiring a
recognised box on the TAKER's side makes the rule unreachable in exactly the deadlock it exists
for.** Having heard anything at all is still strictly more than an empty VLAN can ever show, which
is what makes it enough — and it is the holder's side, where the evidence is available because we
are the ones driving, that carries the strict test.

- **A HOLDER is a segment with a master engine open on the same PHYSICAL port** — `link_port_of`'s
  answer, so a parent and its VLAN children are one port, exactly as the admission already counts
  them. A tap holds nothing (it opens no TX side at all) and a door holds nothing (it has no
  engine).
- **A YIELD IS NEVER A STANDING PREFERENCE.** It is asked for at exactly one point: inside the
  admission, when the budget has ALREADY been found not to fit. On a port with room — a 1 Gbit
  trunk, or any link whose speed cannot be read, which §5d rules is "unknown, never full" — the
  question is not even asked and no neighbour is touched. Measured why: a first cut that yielded
  wherever a segment heard something tore down a live VLAN on a trunk with plenty of room, and
  `tests/hearing-finds-a-segment.sh`'s trunk phase went red on exactly that (2026-09-20).
- **A TAKER must have HEARD something on its own wire** (`reac_hunt_heard_anything`). A segment
  that has heard nothing takes nobody's budget, so two empty segments can never trade a port.
- **EMPTY means the same three facts `port_siblings_served` already trusts**, and no new
  classifier: the master FSM is not past PROBING (`reac_sink_node_past_probing`), no box has been
  recognised (`reac_sink_node_recognized_box`), and no frames are arriving (`reac_segment_heard`).
  Any ONE of them makes the holder non-empty and the yield does not happen.
- **ESTABLISHED, GRANTING, CARRYING, or a foreign master being TAPPED never yields.** Neither does
  a holder **PINNED** by `reac-pw.conf` — the operator answered for that wire, and the one thing
  that may act on a pin is the refusal (§2). That stays the operator's call, and the refusal line
  now tells them it is theirs to make.
- **ALL OR NOTHING.** The yield frees the port only when EVERY holder on it is empty. One
  established holder and the hearing segment is refused, exactly as today — a port with a box
  already on it is not opened up by a second box appearing.
- **A YIELD IS NOT A FLAP.** The yielding segment goes back to LISTENING through the serve-failed
  path the daemon already has (`reac_ifscan_serve_failed`: SEGMENT → LINKED with a retry window),
  which does not touch the carrier-flap counter of the 2026-09-02 amendment (b) and bounces no
  port. The hold-down is untouched.
- **AND IT STAYS LISTENING UNTIL IT HEARS SOMETHING ITSELF.** A yielded segment does not re-elect
  on the silence licence that won it the wire the first time: the licence is an argument about an
  empty wire, and it is just as true five seconds later, so without this two empty segments would
  hand the budget back and forth for ever. `reac_hunt_heard_anything` is the gate — the segment's
  own evidence, not a timer.
- **THE REFUSAL NAMES THE HOLDER.** `E_LINK_BUDGET` (the line was codeless until now), with every
  holder on the port and what each one is: `probing, no box`, `probing, no box, pinned`,
  `established` or `carrying frames`. The operator reading 690 of these should not need a roster
  dump to see which segment to ignore.

### The knob this needs, and why it is one

`link_speed_mbit` reads `/sys/class/net/<port>/speed`, and §5d already rules that a speed it cannot
read is **unknown, never full**. That is right on a rig and untestable in a namespace: a veth
reports 10 000 Mbit/s (measured, private sysfs mount, 2026-09-20), so no arrangement of fake
segments can fill a test port's budget. **`REACPW_LINK_MBIT[_<port>]`** declares a port's rate:
announced at start like every other knob (2026-09-17 ruling), per-port through `reac_conf_lookup`'s
segment layer, host-wide otherwise, and it OVERRIDES sysfs rather than only filling a gap — a NIC
that negotiates 1 Gbit into a 100 Mbit uplink is the operator's to declare, and a knob whose effect
is invisible in the journal would be the obscurity that ruling refuses.

### What this does NOT decide

Whether an empty master should start at all on a port whose budget cannot carry two. It should:
probing is what captures a cold box (§5c, and `reac_knock`'s licence), and the issue leaves it
open on that reasoning. What this amendment adds is that it must be EVICTABLE.

**Proven by** `tests/empty-master-yields-the-budget.sh` (netns, serial): two VLANs on one capped
port, the empty one wins the race, a box then speaks on the other, and the taker reaches
`S_SEGMENT_UP` while the empty holder reads `S_BUDGET_YIELDED` + `S_SEGMENT_DROPPED`; the negative
arm gives the first VLAN its own box, and the second is then refused with `E_LINK_BUDGET` naming
the holder. **Not proven:** the real 100 Mbit trunk that produced the issue.
