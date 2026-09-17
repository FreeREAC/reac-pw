<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# The daemon can BE a box — a fourth role, and a model table that is DATA

Status: RULED by the operator 2026-09-17, verbatim — *"the daemon gains a BOX role, so that a
mixer can use it: receive and send ports to a REAC mixer"*, *"We should be able to emulate ALL
REAC boxes, even the ones we still haven't seen like S-0816 and S-2416"*, *"invent firmware and
REAC versions, also the name"*, *"test if we can emulate a 40 channels input or output box"*.
Normative for the `box` role, for libreac's box-model table and for what the daemon declares to a
mixer master. AMENDS
[`2026-09-16-segments-and-roles-are-autodetected.md`](2026-09-16-segments-and-roles-are-autodetected.md)
§3a (the conf grammar gains `role = box` and `model =`) and its §B roster vocabulary; everything
else in that spec — the discovery rules, the drop-in order, the provenance, the one-`stat`
re-read — stands unchanged and is not restated here.

## 0. Why the role exists

Operator, 2026-09-17: *"so that a mixer can use it: receive and send ports to a REAC mixer"*, and,
the same day, *"being a REAC box could be a way to connect to other systems like a Dante patch or
a Midas patch"*. The box role is a BRIDGE: a Roland desk sees a stagebox, and what is behind that
stagebox is whatever this host can carry — a PipeWire graph, a Dante patch, a Midas stage box, a
recorder. That is also why §6a rules the head-amp onto a real preamp rather than a number: the
thing on the other side of the bridge has gain, and the desk must be able to drive it.

## 1. What a box role IS, and why it is not a new engine

A REAC pairing has two ends, and a stagebox is the NON-driving one: the master drives the
cdea/cfea establishment, grants, and owns the pace; the box answers, declares its geometry,
returns its inputs upstream and takes the master's downstream. `reac_slave` is already exactly
that end — it floods filler while unlinked, is granted, locks to the master's frame arrival as its
slot clock, announces a model with `reac_ctrl_build_config_announce`, and sends the identity
record when its model row asks for one (`reac_slave.h`, `reac_ctrlblk.h`'s builders).

**So `box` is not a second engine and MUST NOT become one.** It is the slave engine plus two
declarations the slave role today takes from elsewhere: WHICH MODEL we present, and which end of
the PipeWire graph is which. A fork of `reac_slave` (or of `fake_box`, which is the same engine's
capture-shaped twin used to enrol the daemon's own tests) would be a second answer to a settled
question, and the first divergence would be a wire fact that only one of them got right.

`box` and `slave` differ in INTENT, which is a fact about us and not about the wire:

| | `slave` | `box` |
|---|---|---|
| the peer | a desk, or a box in M mode, that we joined because the wire already had a master | a mixer we are DECLARING ourselves a stagebox to |
| what we present | the width we were configured with, model resolved by width | a TABLE ROW, named in the conf, with its own identity |
| PipeWire | the console's own capture/playback pair | a source of what the mixer sends us, a sink of what we send it (§5) |
| head-amp | passed to the console's head-amp state | ACKNOWLEDGED, and parked (§6) |
| elected by `auto`? | yes — a master on the wire elects it | **never** (§7) |

## 2. RULING — the model table is DECLARED DATA, and every wire block is SYNTHESISED from it

`reac_ctrlblk.c`'s `BOX_MODELS[]` already holds the three byte-verified rows (S-1608, S-0808,
S-4000S) as CAPTURED 32-byte blocks: the config-announce, the two cold-connect join records, the
two identity-page records, and, for the 0x84 family, the two-frame ASCII name record. A model
nobody has captured has no such bytes, so as long as a row IS its captured bytes, an unseen model
cannot be a row — it can only be code.

**Every block in the table is therefore DERIVED from a row's declared facts**, by the grammar the
corpus already pinned, and the captured bytes become the ORACLE for that derivation rather than
its source:

- **the config-announce block** (`01 03 00 10`) is `selector`, the head-amp chassis strap at
  `block[7]`, then the twelve-slot port table at `block[8..19]` that `reac_ports.h` already
  decodes — `0x02` per 4 inputs, `0x01` per 4 outputs, `0x03` for each empty slot, always twelve
  slots for the 48-channel ring — then the row's 11-byte model tail, then the block check byte;
- **the identity page** (DT1 tag `0x0500`, `reac_identity.h`) is three records and all three are
  the row's declared identity: `addr 0x0000` is the firmware as four decimal digit bytes,
  `addr 0x0600` is the REAC version as four `u16be` (reserved, major, minor, patch), `addr 0x1000`
  is one `name_kind` byte and the 16-byte NUL-padded ASCII name, split across the link-4 FIRST and
  LAST fragments at 10 + 6 bytes;
- **two checksums, in this order** — the Roland DT1 inner checksum `(128 - Σ(address+data)) mod
  128`, then the REAC block check byte `(256 - Σ block[0..30]) mod 256`, so that the 32 bytes sum
  to zero mod 256. This is the order `reac_ctrlblk.c` already stamps in and the reason it is
  structural there.
- **the two cold-connect JOIN records** (`0014`, `0013`) are model-generic in the whole corpus and
  are the table's constants, not a row's.

**The derivation is PROVEN by the corpus, not asserted:** for each captured row, the synthesiser
must reproduce every one of its captured blocks BYTE FOR BYTE. That test is the whole licence for
trusting a row nobody has ever seen on a wire — an unseen model is a table row because the same
generator, fed the seen models' facts, reproduces the seen models' bytes.

### 2a. A row's grammar

```
token            the conf/CLI word: "s1608", "s4000s-0832", "fr4040"
display          the human label
in_ch / out_ch   the declared widths; multiples of 4, in_ch + out_ch <= 48 (twelve slots)
selector         the config-announce model-family byte (0x82 / 0x84)
headamp_strap    block[7]; the box's own head-amp CH base is strap * 0x10 (reac_ports.h)
tail             the 11 model-tail bytes after the port table (zero unless a capture says otherwise)
fw_milli         firmware x1000: 2200 is "2.200"
reac_major/minor/patch   the REAC version the box claims: (2,3,2) prints "2.302"
name             the ASCII identity name, or "" for a family whose selector already names it
origin           CAPTURED (a real box's bytes are the oracle) or DERIVED (nobody has seen one)
```

`in_ch`/`out_ch` are the ONE place a width is declared. Nothing derives a width from a model
elsewhere, and nothing derives a model from a width except `reac_box_model_by_channels`, which
keeps its existing meaning for CAPTURED rows only — a derived row must be asked for BY TOKEN, or
an experiment row would start answering for a real box's width.

### 2b. The rows

| token | display | in/out | origin | identity |
|---|---|---|---|---|
| `s1608` | S-1608 | 16/8 | CAPTURED | Roland: fw 2.200, REAC 2.302 |
| `s0808` | S-0808 | 8/8 | CAPTURED | Roland: fw 1.003, REAC 1.000, name `S-0808` |
| `s4000s` | S-4000S-3208 | 32/8 | CAPTURED | Roland: fw 2.500, REAC 2.102 |
| `s4000s-0832` | S-4000S-0832 | 8/32 | DERIVED | Roland-shaped (the same chassis, the other strap) |
| `s0816` | S-0816 | 8/16 | DERIVED | ours |
| `s2416` | S-2416 | 24/16 | DERIVED | ours |
| `s4000d` | S-4000D | 0/32 | DERIVED | ours |
| `s4000m` | S-4000M | 32/0 | DERIVED | ours |
| `s4000h` | S-4000H | 16/16 | DERIVED | ours |
| `fr4000` | FreeREAC 40 in | 40/0 | DERIVED | ours — the experiment row |
| `fr0040` | FreeREAC 40 out | 0/40 | DERIVED | ours — the experiment row |
| `fr2020` | FreeREAC 20/20 | 20/20 | DERIVED | ours |

The S-4000S row is named for the split it declares (`3208` = 32 in / 8 out); its `0832` sibling is
the same chassis strapped the other way and is DERIVED because no capture of one exists. The
S-4000D/M/H widths are what their names say and are DERIVED in the strict sense: they are a
GUESS at a real Roland product's geometry, and a row being in the table is not a claim that a
Roland desk will accept it — §9 says what would prove that.

**The 40-channel rows are the operator's experiment and carry NO Roland model behind them.** 40 is
the audio fabric's full width (`REAC_BOXREG_FABRIC`), which is not 48: the port table spans 48
channels but the downstream frame carries 40 slots, so 40/0 and 0/40 are the widest rows the
fabric can actually carry and `fr2020` is the widest symmetric one.

## 3. RULING — the declared identity is OURS by default

A row whose `origin` is DERIVED declares a **FreeREAC** identity: the name is ours (`FR-…`), the
firmware is the daemon's own version (`1.014` for reac-pw 1.0.14 — one number, so a box on a desk
can be traced back to the build that emulated it), and the REAC version is **major 9**, minor and
patch from the daemon's own, so it can never be mistaken for a Roland 1.x/2.x box.

A row may ask for a **Roland-shaped** identity instead, and the three captured rows do — they are
imitations of real boxes and their whole point is to be byte-identical. `s4000s-0832` asks for one
too, because it is the same chassis and the same firmware.

**This is a real gamble and it is named here rather than discovered on the rig:** a mixer that
gates enrolment on a known REAC version will refuse `9.xxx`, and the fix is one field in one row.
Nothing in the corpus shows a desk reading the identity page before granting — the M-200i displays
it AFTER the box is enrolled — so the default is honesty, and §9's rig step is where it is
settled.

## 4. RULING — the conf names the role and the row

`2026-09-16-segments-and-roles-are-autodetected.md` §3a's grammar, unchanged, with one new value
and one new key:

```ini
[segment enp131s0.11]
role  = box          # auto | master | slave | tap | box   (default: auto)
model = s1608        # a token from the table; required when role = box
```

- `model` is meaningful ONLY under `role = box`; on any other role it is refused BY NAME (spec
  §3a's rule: reported, never fatal), because a key that is read on one role and ignored on
  another is a trap with no upside.
- `role = box` with no `model` is refused by name and the segment falls back to `auto`. There is
  no default model: a box that declares the wrong width to a mixer is a patch that silently lands
  on the wrong channels, and an absence is a fact (`false-signals` §5).
- An unknown `model` token is refused by name, and the refusal LISTS the table's tokens. A table
  of rows nobody can spell is a table nobody can use.

## 5. RULING — the PipeWire pair is the MIXER's view, mirrored

A box's ports are named from the BOX's side and the graph's are named from ours, so the two are
crossed and the crossing must be stated once:

- **`reac-capture` (a SOURCE) carries what the MIXER SENDS US** — the box's `out_ch` outputs, the
  master's downstream frame. This is what an operator patches to their monitors, their recorder or
  a console strip;
- **`reac-playback` (a SINK) carries what WE SEND THE MIXER** — the box's `in_ch` inputs, our
  upstream frame. What is played into it arrives on the mixer's input channels.

So a `role = box, model = s1608` segment publishes a source with 8 ports and a sink with 16, which
is the mirror image of the same row under `role = master`. The widths come from the ROW and from
nothing else — not from the master's grant, which a real box also ignores for its own geometry.

The no-box-no-node amendment holds and gains its fourth case's twin: **a box-role segment has
something to carry the moment its row is declared**, exactly as a `--box MODEL` pin does, so its
pair is published at start and not when a mixer appears. A stagebox that only exists once a desk
is powered is not a stagebox.

## 6. RULING — head-amp commands are ACKNOWLEDGED and APPLIED AS A DIGITAL TRIM

A mixer drives a box's preamps: gain, pad and phantom per channel, over the head-amp records
`reac_headamp_tx.h` already builds and `reac_ctrl_parse` already decodes. We have no preamps —
and the slave engine already answers this question, which is the answer the box role takes rather
than inventing a second one:

- **SENS and PAD become the equivalent DIGITAL gain on the channels we return upstream**
  (`reac_slave_headamp_gain`: a sensitivity of S dBu is a preamp gain of −S dB, with the pad
  folded in, carried in centi-dB). That is what a real box's preamp does to the signal it sends,
  so a mixer's head-amp control is not inert against us — it moves the audio the mixer receives,
  which is the only thing it could honestly move.
- **PHANTOM is recorded and actuates nothing.** +48 V is a voltage, not a gain. It is kept as
  state, published where the other two are, and claimed nowhere: no hardware claim ever comes
  from a soft value on this project (`CLAUDE.md`, and the head-amp rulings of 2026-09-14).
- **A record for a channel outside our declared width is dropped**, by the row's own head-amp
  base — `model_base + (input − 1)`, and the base is the ROW's chassis strap, not a per-width
  table (`reac_ports.h` retired that table and the box role conforms: the engine takes
  `strap × 0x10` from the declared row).

### 6a. RULING (operator, 2026-09-17, mid-lane) — the head-amp must reach a REAL preamp

*"Being a REAC box could be a way to connect to other systems like a Dante patch or a Midas
patch, that is why we need preamp."*

**This is what the box role is FOR, and it changes the standing of the digital trim above: the
trim is the FALLBACK, not the answer.** When the daemon fronts real inputs — a Dante patch, a
Midas stage box, a local interface — the mixer's SENS/PAD/phantom records must reach THAT
device's own preamp, so a Roland desk drives the gain of a stage box it cannot speak to. A
console operator turning up channel 3 must move a real head amp, not a number in our ring.

So the head-amp path has three tiers and a row says which it is on:

1. **a declared local preamp door** — the segment names the device whose preamp answers, and
   SENS/PAD/phantom are actuated there. THE TARGET, and the reason for the role;
2. **the digital trim** — what the engine does today, correct for a virtual box and honest about
   being all it is;
3. **phantom, always state-only unless tier 1 answers it**, because +48 V is a voltage and this
   project never claims one from a soft value.

**Tier 1 is NOT BUILT in this lane** and needs its own spec: which door (openmixer's head-amp
controller? a local ALSA/USB mixer element? the Dante/Midas control protocol?), how a row declares
it, what a refusal looks like when the device cannot do pad or phantom, and how a restart
re-asserts it — the restart-sweep rule of 2026-09-14 applies unchanged, and a divergence between
what we publish and what the preamp is engaged in is a P0 on this rig.

Refusing them is not an option: a real box answers, and a master whose head-amp writes are
ignored retries them for as long as it runs.

## 7. RULING — `auto` NEVER elects `box`

`reac_hunt`'s verdicts are unchanged and none of them is `box`. A silent wire elects MASTER, a
desk elects SLAVE or TAP, a box master elects SLAVE. `box` joins `tap` as an EXPLICIT-ONLY intent
(`reac_role.h`), for the mirror-image reason: a mixer never wants a surprise stagebox appearing on
its fabric and taking channels, and the cost of a wrong guess is a desk whose inputs move.

**And the converse: a box NEVER DEFERS.** The deferral that turns an `auto` segment into a tap
when a foreign desk is heard (`segment_defers_as_tap`, master-arbitration's eighth amendment)
does not apply to a box, for the same reason it does not apply to a slave — a desk on the wire is
exactly who a box is there for. Measured before it was fixed: with a mixer on the far end the
roster of a segment pinned `role = box` read `tap`, the slave engine was never opened, and the
daemon sat silent behind a correct-looking configuration.

## 8. Proven, and by what

| rule | test |
|---|---|
| §2: the synthesiser reproduces every captured block of every CAPTURED row, byte for byte | `libreac tests/test_reac_box_table.c` |
| §2: the port table, the two checksums and the 12-slot sum hold for every row | `libreac tests/test_reac_box_table.c` |
| §2/§3: every row's identity page round-trips through `reac_identity_ingest` to the row's own facts | `libreac tests/test_reac_box_table.c` |
| §2b: the 40-channel rows are legal rows and declare 40 slots | `libreac tests/test_reac_box_table.c` |
| §4: `role = box` + `model =`, refusals by name, `box` without a model | `reac-pw tests/test_reac_segconf.c` |
| §7: `auto` never resolves to box, on any wire the hunt can see | `reac-pw tests/test_reac_role.c` |
| §1/§2/§3: the declared row REACHES THE WIRE — port table, strap, firmware, REAC version, name — decoded off the peer end of a veth by a sniffer that is not this daemon | `reac-pw tests/box-declares-its-row.sh` |
| §5: both nodes are published, and the roster reads `box` with the row's model and width | `reac-pw tests/box-declares-its-row.sh` |
| §1: a REAC master on the other end ENROLS us as the row we declared (`established`, model `s1608`) | `reac-pw tests/box-declares-its-row.sh` arm A |
| §7: a box does not defer as a tap when a master is heard | `reac-pw tests/box-declares-its-row.sh` (both arms would read `tap`) |

## 8b. What the first run MEASURED, including the two things it broke

`tests/box-declares-its-row.sh`, on a veth pair inside a private namespace, with this daemon's own
master side as the mixer. Both arms' numbers are read off the wire by a raw-socket sniffer that
decodes the declaration the way a desk must:

| | arm A `model = s1608` | arm B `model = fr4000` |
|---|---|---|
| frames | 113 957 (10 920 of them broadcast flood) | 74 920 (10 920 flood) |
| frame size | 628 B — a 16-channel box frame | 1492 B — a 40-channel box frame |
| declaration | selector `0x82`, strap 2, **16 in / 8 out**, block sums to 0 | selector `0x84`, strap 0, **40 in / 0 out**, sums to 0 |
| identity | firmware **2.200**, REAC **2.302**, no name record | firmware **1.014**, REAC **9.014**, name **FR-4000** |
| the mixer | **`established`, model `s1608`** — and its clock locked to our counter slope, naming "S-1608 (16 in / 8 out)" | grants (10 JOINs counted) and does **not** sustain presence |

Two defects it found, both fixed in this lane and both invisible to every unit test:

1. **A box-role segment deferred as a TAP** the moment the mixer spoke (§7's new paragraph). The
   configuration was right, the roster said `tap`, and no engine ever opened.
2. **The emulated box announced from this NIC's own MAC**, not a Roland OUI, and our master
   counted `rx_box_frames=0` against a box that was flooding — it logged `model=unknown`. Every
   box in the corpus announces from a Roland OUI and the recognisers key on it, so the box role
   now takes the same Roland-OUI stand-in the box-master path already used.

**The 40-channel experiment's answer, so far: it declares and it is granted, and presence is not
sustained.** Our own master reads `rx_joins=10` and then `no sustained presence`. That is one
master's behaviour and not the protocol's verdict — but it is the first evidence either way, and
it says the interesting question is upstream presence at the fabric's full width, not the
declaration.

## 9. NOT proven here — the rig day

**No Roland mixer has accepted us.** Every test above stands on veth pairs inside a private
namespace, against our own master and our own `fake_box`-shaped peer, which proves the daemon's
decisions and proves nothing about an M-200's firmware. Three things are open until a real desk
answers, in the order they will fail:

1. whether an invented REAC version (§3) is read before the grant;
2. whether a DERIVED width the desk has no product for is granted at all, or granted at some
   other width;
3. whether the 40-channel rows enrol, which is the operator's stated experiment and has no Roland
   precedent anywhere in the corpus.

The rig step is `~/.config/reac-pw/reac-pw.conf.d/99-local.conf`, the segment carrying the M-200,
and `libreac tools/m200-compare.sh` — the exact lines are in the lane's report.

## Amendment 2026-09-17 — a real S-4000H declared itself, and its declaration breaks two guesses

An **S-4000H, 8 in / 32 out**, alone on VLAN 13, was captured declaring itself to our master
(`vlan13-0832.pcap`, 4 s, `enp131s0.13`, box `00:40:ab:c4:25:80`). Its config-announce, as sent:

```
01 03 00 10 | 84 | 00 00 | 00 | 01 01 01 01 01 01 01 01 00 00 03 03 | 00 03 00 00 00 01 00 00 00 00 00 | 56
  link/len  | sel|       |strap|          the twelve port slots      |        the model tail           | ck
```

Three facts come out of it, and two of them refute something this spec had guessed.

**a. `0x00` IS AN INPUT GROUP — a fourth port-slot code, now captured.** `reac_ports.h` listed
three codes and refused everything else, and that refusal is what kept this box off the graph:
`reac_ports_parse` returned −1, so `reac_master_set_box` was never called, so the master held an
ungranted window until `grant_dwell` and dropped back to PROBING with `REAC_M_DROP_BOX_UNKNOWN` —
for minutes, exactly as the operator saw (`state=probing model=none role=master width=0/0`). The
table above reads 8 output groups (32 out) + 2 of `0x00` + 2 empty = twelve, and the operator's
ruling the same day names the chassis 8 in / 32 out: the `0x00` groups are the box's 8 inputs.
What DISTINGUISHES `0x00` from `0x02` is NOT decided here — one capture, one chassis, and a
splitter's inputs may well be marked apart from a head-amp-owned input. §9 gains the rig step.

**b. THE TABLE IS A FABRIC PLACEMENT, NOT A SORTED LIST.** All three previously captured rows put
their input groups first, so the synthesiser sorted in-then-out and nobody could tell the two
apart. This chassis places its OUTPUT groups first. A row therefore declares its layout:
`port_layout` — `IN_FIRST` (the three Roland rows, and the default) or `SPLIT_OUT_FIRST` (this
chassis: outputs first, inputs marked `0x00`). The two halves of that shape were captured
together and no capture shows them varying apart, so they are ONE declared fact and not two
flags — a row may not mix a layout nobody has seen.

**c. §2b's `s4000h` row was a GUESS at 16/16 and is replaced by the capture.** The row is now
`s4000h` | S-4000H-0832 | **8/32** | **CAPTURED** | Roland, identity page NOT captured. The
sibling `s4000s-0832` stays DERIVED: it is a different chassis strap and this capture says
nothing about it.

**THE IDENTITY PAGE IS ABSENT FROM THIS CAPTURE, AND ABSENCE IS RECORDED AS ABSENCE.** In 4 s the
box sent its config-announce and the three cold-connect JOIN records (`0014` join, `0000`
head-mark, `0013` box-ready — byte-identical to the S-1608's, the table's constants) and a box
heartbeat. It sent NO `0016`/`001a` inventory record, no link-4 name fragment and no DT1 `0x0500`
reply: firmware, REAC version and ASCII name are UNKNOWN for this box. They are left at zero and
`reac_box_model_block` emits no identity block for a row whose `fw_milli` is zero — a row may not
fabricate a page it has never seen, and copying the S-4000S's 2.500/2.102 would have been exactly
that. The identity page is polled by the grant sweep's group B, so the likely reason it is absent
is that we never granted; the capture that would settle it is named in §9.

**A WIDTH NEVER NAMES THIS ROW.** `s0808` is 8/8 and `s4000h` is 8/32, so a width of 8 can no
longer pick one — and `reac_box_master_model(width)` answered the FIRST row of that width. The
slave path is the only caller and it has nothing else to go on: a stagebox strapped to master mode
sends no declaration at all, so a width is its whole evidence. The rule is therefore that a row
with no identity page (`REAC_BOX_DECLARED`) does not answer a width; `s0808` keeps the number it
has always had, and the S-4000H is named by its DECLARATION, byte for byte, on the master path
where it sends one. **And if two FULLY CAPTURED rows ever share a width, the answer must be
neither** — that is law here, and deliberately not code yet: the pair that would exercise it does
not exist, and a branch no test can reach is decoration. The capture that creates the pair writes
the branch.

Either way the number is never the name. This box is why that is not academic: it declares 8
inputs and its upstream frames are **1204 B = 32 channels**, so a length-derived width and the
declared width disagree by 24 on the live wire.

**ONE MAC, ONE VERDICT.** The daemon logged the same box twice — `box … (8 ch)` off the 340 B
config-announce and `unknown … (32 ch)` off the 1204 B broadcast flood — because the sniffer's log
printed the per-FRAME sighting. A broadcast filler is deliberately role-`unknown` (a master's
downstream is byte-identical in kind), so a box that floods will always produce that second line
while the sighting is what reaches the log. `reac_hunt_observe` now answers with the disco TABLE's
entry for that MAC — the corroborated verdict, where facts only sharpen — so one box is one line.

### §9 gains three rig steps for this chassis

4. **Which upstream slots carry the 8 inputs.** The box returns 32 channels; a 4 s capture cannot
   say which 8 are its preamps (nothing was plugged in, and the pre-grant 340 B frames are
   digitally silent). Inject a tone into input 1 and require the level to follow it — a null from
   an un-granted box is not evidence (`CLAUDE.md`: prove the injection landed).
5. **The identity page**, once the grant sustains: `tcpdump -i enp131s0.13 -w … ether proto 0x8819
   and ether host 00:40:ab:c4:25:80`, 20 s from a box power-cycle, which spans the grant sweep's
   group-B poll. The row's `fw_milli` / `reac_*` / `name` stay zero until it lands.
6. **Whether `0x00` marks a head-amp-less input.** Send a head-amp record at strap base 0x00 and
   look at the preamp, physically — never at a soft meter.

## Amendment 2026-09-17 (second) — RULING: any stagebox enrols, on what it declares

**Operator, 2026-09-17, verbatim:** *"With the knowledge we have of the protocol, we should be
able to enrol any stage box."*

The sizing path already intends this — `reac_master_set_box` takes the width the box DECLARED and
the model table only NAMES it, so an unnamed box is sized and granted. One line broke it for every
box we have not captured: `reac_ports_parse` refused the WHOLE twelve-slot table on the first byte
outside `{01,02,03}`, and a refusal there means no `set_box`, no grant, and a roster stuck at
`probing`. The S-4000H is the proof — one unfamiliar byte, and a fully declaring Roland box could
not join for minutes.

**A SLOT CODE SPEAKS FOR ITS OWN FOUR CHANNELS AND NO OTHERS.** The decoder now counts an unknown
code as an UNKNOWN group — reported as `unknown_ch` and the first code seen — and answers with the
geometry it CAN read. The box enrols at that width.

- This is not the guess the old refusal existed to stop. That guess was an OVER-claim: naming a
  box, or sizing it, from something other than its declaration, so the grant claims slots the box
  does not own and the head-amp lands 32 slots off with every gate green. An unknown group is an
  UNDER-claim: we enrol the groups we can read, the box's own frame width still carries its audio
  (`reac_rx` takes the width off the frame, never off the grant), and nothing is addressed that
  was not declared.
- **It must be loud.** `REAC_PEV_RECOGNIZED` carries the unknown group count and the first unknown
  code, and the master's log names them, so an unknown code reads as "capture this byte", never as
  a silent narrowing. Recoverable and loud beats plausible and wrong.
- A declaration that is not a declaration is still refused outright: wrong link, wrong segment
  bits, wrong opcode. Those say "this block is not a box declaring itself", which is a different
  fact from "this box has a group I do not recognise".

What is still refused, and stays refused: **naming** a model from anything but a byte-exact
declaration, and deriving a width from a frame LENGTH. Enrolling any box is a statement about
GEOMETRY, never about identity.

## Amendment 2026-09-17 (third) — the chassis numbering, and what the port table's ORDER is not

**Operator, 2026-09-17:** *"inputs are tagged 1-8 and outputs 9-17 and so on in 4 groups"* — the
S-4000H numbers its 8 inputs 1–8 and its 32 outputs from 9 upward, in groups of four.

So the fabric placement is INPUTS FIRST, rows 1–8, outputs from row 9 — which is exactly what the
allocator already grants (`0x41` per input group front-packed from the base, `0xc3` per non-input
group back-packed). Nothing in the grant changes.

**It does correct the first amendment's wording.** The twelve-slot table's ORDER is the order this
chassis WRITES ITS DECLARATION in, and not the fabric placement — the S-4000H writes its output
groups first and numbers its inputs first, so the two are demonstrably not the same thing.
`reac_box_port_layout` is therefore a declaration-order fact and is named and documented as one.
Nothing was decided from the order; the widths come from COUNTING codes, which is order-free.

**And it names the upstream slots.** With the chassis numbering plus the braid's ascending channel
order (`reac_upstream.h`: input N is wire channel N−1), the 8 inputs are channels 0–7 of the
box's 32-channel return. The capture is consistent with a group structure — the 1204 B frames
repeat one 8-channel pattern four times — but consistency is not proof, so §9 step 4 stands as
written: inject a tone into input 1 and require the level to follow it.

## Amendment 2026-09-17 (fourth) — the M-200 is the oracle, and it corrects two of the above

Three captures of the SAME box (`00:40:ab:c4:25:80`) on a real M-200 (`00:40:ab:c9:cc:03`,
44.1 kHz, 3675 fps, VLAN 12): `m200-s4000h-enrol.pcap` (60 s, steady state),
`m200-s4000h-coldboot.pcap` (90 s, power-cycle) and `m200-s4000h-replug.pcap` (60 s, cable
pull). They settle what our own 4 s could not.

**a. THE ROW IS FULLY CAPTURED, AND ITS NAME IS `s4000s-0832`.** The cold boot carries the box's
one config-announce (t=+53.704) — byte-identical to the block captured on VLAN 13, which is the
independent second wire that block needed — and, at t=+57.438, its identity page: `cc0016`
firmware `"2500"` and `cc001a` REAC `0000:0002:0001:0002` = 2.102, both **byte-identical to the
S-4000S-3208's captured records**. The M-200 displays "S-4000S, 08 in / 32 out, fw 2.500, REAC
2.102" and it is right: the box sends NO name record, so the 0x84 selector's default label is all
a desk has. One chassis, two straps. The `s4000h` token is gone; the H is a front-panel label.

**b. THE 32-CHANNEL RETURN WAS A STATE, NOT A CHASSIS FACT — the first amendment over-read it.**
Granted by the M-200 the box returns **340 B, 8 channels**, for all 217 905 frames of the steady
state and from the first frame of the cold boot. The 1204 B frames it sent US are what it does
when its declaration goes unanswered. `reac_box_model_upstream_width` therefore stays the
declared input count, and §9 step 4 (which upstream slots carry the 8 preamps) is ANSWERED for
the granted case: there are only eight, ascending, as `reac_upstream.h` already says.

**c. OUR GRANT IS NOT THE DEFECT, AND HALF OF IT IS BYTE-IDENTICAL TO THE M-200's.** The desk's
ENROLL group map for this box is `01 03 00 0d 10 04 02 41 00 00 00 00 00 c3 c3 c3 c3 …` — ONE
`0x41` input group and FOUR `0xc3`, which is exactly what `set_enroll_width(8)` emits, byte for
byte. Its scene push is 686 chunks, which is exactly what ours sends. The whole divergence is
that we never reach the grant at all: the M-200's cfea goes `boxes=0` → `boxes=1` after its grant
sweep, and ours stayed `boxes=0` because the port decoder refused the declaration. Hypothesis (c)
of the brief — "our grant asks the wrong width" — is REFUTED by these bytes.

**d. NOT OURS TO FIX HERE, AND MEASURED: THE M-200 ADDRESSES THIS BOX'S PREAMPS AT CH 0x20.**
Twenty-four head-amp records at t=+57.4, CH `0x20`–`0x27`, three params each — for a box whose
config-announce carries `block[7] = 0x00`, which `reac_ports.h`'s strap law reads as base `0x00`.
The law holds for the three boxes it was derived from (S-0808 0x00, S-1608 0x20, S-4000S 0x00) and
does not predict this one. **Every head-amp record we send this chassis would land 32 rows away**,
which is the silent-48 V failure mode that law exists to prevent — so it is recorded here as a
measured divergence with a rig step, and NOT patched from one capture: the base may follow the
box's OUTPUT count, the granted placement, or a strap byte we are reading wrong. The step is
`REAC_HEADAMP_PHANTOM` at 0x20 versus 0x00 on the live unit, confirmed by looking at the preamp.

**e. THE CFEA WIDTH BYTE IS CORROBORATED BY A SECOND BOX.** The M-200 announces the ENROLLED
box's input width: `0x08` beside this chassis, `0x10` beside an S-1608 on the same desk
(`m200-s1608-replug.pcap`, steady state, `boxes=1`). `reac_master_set_box` stamps
`cfg.out_channels = in_ch`, which is that byte, so an admitted 0832 makes us announce `0x08`
exactly as the desk does — asserted in libreac's `tests/test_box_0832.c` arm 4.
