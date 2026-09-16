<!--
SPDX-License-Identifier: GPL-3.0-or-later
Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
-->
# Node properties and params — the graph vocabulary

reac-pw's whole control surface is its PipeWire nodes. There is no control socket, no D-Bus
name and no CLI: a client **reads** node properties and **writes** node params, and the only
gate is PipeWire session access — reac-pw runs as a user unit on the operator's socket, so
any client of that session reaches it and nothing else reaches it at all.

Two nodes per segment:

| node | what it is |
|---|---|
| `reac-playback[.<segment>]` | `Audio/Sink` — the MASTER role's door. It carries the write params and the head-amp answer, and it exists only where reac-pw can send. |
| `reac-capture[.<segment>]` | `Audio/Source` — the decoded wire. It mirrors the box badge and carries the segment's role/arbitration answer in every role. |

`<segment>` is the interface name (`REAC_NAME` overrides it). A client keys on the
`reac.segment` property rather than on the node name, so the same segment stays addressable
when the role swaps and the other node becomes its door.

## Writing — `SPA_PARAM_Props`

Every write is one `set-param` on `SPA_PARAM_Props`, carried in SPA's extensible
`SPA_PROP_params` bag as alternating (String key, value) pairs. The node enumerates the
vocabulary in its `SPA_PARAM_PropInfo`, so a client discovers it:
`pw-cli enum-params <id> PropInfo`.

| key | value | node | effect |
|---|---|---|---|
| `reac.headamp.<wireCh>.phantom` | `0` \| `1` | playback | 48 V on one box preamp |
| `reac.headamp.<wireCh>.pad` | `0` \| `1` | playback | the box's own −20 dB pad |
| `reac.headamp.<wireCh>.sens` | `0 .. reac.headamp.sens.max` | playback | preamp sensitivity, 1 dB per step under `dBu = -10 - value + (pad ? 20 : 0)` |
| `reac.cfg.rate` | `44100` \| `48000` \| `96000` | playback, capture | the REAC pace this segment runs at; an accepted change re-clocks the segment and the box re-enrols |
| `reac.cfg.role` | `0` (master) \| `1` (slave) | playback, capture | which end of the desk↔stagebox pairing this segment presents |

`<wireCh>` is the absolute REAC wire channel in decimal — `reac.headamp.base + (box input −
1)`. Values arrive as Bool, Int or Float, so a toggle, a spin button and a slider each send
their natural encoding. A write from a bare shell is one command:

```
pw-cli set-param <playback-id> Props '{ params = [ "reac.headamp.34.phantom", 1 ] }'
```

`set-param` exits 0 whatever the node does with the cell. **The answer is a property**, and
that is the reason each write door below has a `.state` / `.refused` pair beside it.

## Reading — the head-amp door

All on `reac-playback[.<segment>]`, the node that consumes the control keys, so one lookup
yields the shape, the address, the travel, what is asserted, and why a write was refused.

| property | value | meaning |
|---|---|---|
| `reac.headamp.channels` | decimal | preamp-capable box inputs; `0` until a model is recognised |
| `reac.headamp.caps` | `phantom,pad,sens` | the capability tokens a client builds its rows from |
| `reac.headamp.base` | decimal, or `none` | the box's announced chassis strap — the wire address of its first input |
| `reac.headamp.sens.max` | decimal (`55`) | the top step of the sens travel. A client renders the range it receives; a model with a different travel needs no new client |
| `reac.headamp.asserted` | `ch:param=value,...`, `""` when nothing is set | the cells **this daemon** is putting on the wire, `param` being `0` phantom, `1` pad, `2` sens |
| `reac.headamp.state` | `applied` \| `unavailable` | whether this segment has preamps reac-pw can address at all |
| `reac.headamp.refused` | `none` \| `no-box` \| `box-master` \| `no-base` \| `bad-key` \| `out-of-range` | why a write does not reach the wire |

`asserted` is a readback of **our** assertions, never a report from the box: the protocol has
no head-amp readback in the other direction, so nothing on the wire ever confirms a cell.
It outlives a box drop, because it is exactly the table reac-pw replays at the next
establishment — the one mechanism that restores 48 V after a power-cycle.

The refusal codes, and which are standing facts:

| code | it means | standing? |
|---|---|---|
| `no-box` | no model is recognised, so there are no preamps to address | yes, until a box enrols |
| `box-master` | the box's REAC Mode switch is on M. Its preamps are configured through its serial port and no head-amp record exists on that wire in either direction | yes, for that segment |
| `no-base` | a box, but no announced chassis strap. There is no wire address, and `0` is not a safe guess — it addresses an S-1608's preamps 32 slots low, silently | yes, until the strap arrives |
| `bad-key` | the key does not address a cell: a non-numeric channel, no `.` after it, an unknown param name | no — only an arriving write produces it |
| `out-of-range` | it addresses a cell the wire does not have, or a value the range does not admit | no — only an arriving write produces it |

A refusal moves nothing: the cell reaches no send table, so there is no setting waiting to be
pushed at a box that arrives later.

## Reading — the segment

| property | node | meaning |
|---|---|---|
| `reac.segment` | playback, capture | the segment's name; what a console keys its row on |
| `reac.link-state` | playback, capture | the box link as one word |
| `reac.box-model`, `reac.box-width`, `reac.box-source` | playback, capture | the recognised model, its `in×out` width, and whether it came off the wire |
| `reac.box.mac`, `reac.box-firmware`, `reac.box.reac_version`, `reac.box-hw` | playback, capture | the box's own address and identity page |
| `reac.rate`, `reac.rate.source`, `reac.rate.drivable` | playback, capture | the pace this segment runs at, whether it was asserted or taken by convention, and the rates it can drive |
| `reac.cfg.rate.state` / `reac.cfg.rate.refused` | playback | the answer to a `reac.cfg.rate` write |
| `reac.role` | playback, capture | the role this segment is RUNNING, never the latest request |
| `reac.cfg.role.state` / `reac.cfg.role.refused` | playback, capture | the answer to a `reac.cfg.role` write |
| `reac.master.state`, `reac.master.mac`, `reac.master.refusal`, `reac.master.conflict`, `reac.master.rival.kind`, `reac.pace.source` | capture | who drives this wire, and why we did or did not take it |
| `reac.discovery.scope`, `.state`, `.seq`, `.devices` | playback | what this NIC has heard |
| `reac.health.*` | playback | the pacer's own telemetry — see [HEALTH-TELEMETRY.md](HEALTH-TELEMETRY.md) |

## Reading — the ROSTER, including the segments that have no node

The two nodes above exist only where there is **something to carry** — a box recognised, a
master to tap, or a width the operator pinned. A segment that is probing an empty wire has
no node at all, by ruling. So the daemon publishes ONE more node, for itself:

| | |
|---|---|
| `node.name` | `reac-pw` |
| `media.class` | `Reac/Roster` — a class no session manager has a rule for, so nothing links or routes it |
| ports | none |
| find it by | the property `reac.roster = 1`, never the name |

Its properties are the roster, one index-keyed group per segment the daemon runs:

| property | meaning |
|---|---|
| `reac.roster.n` | how many groups there are |
| `reac.roster.<i>.name` | the interface name — the segment's identity, and what a reader keys on. The index is an ORDER (byte order of the names), not an identity |
| `reac.roster.<i>.state` | `probing` \| `established` \| `slave` \| `tap` \| `refused` \| `ignored` |
| `reac.roster.<i>.model` | the recognised box in `reac.box-model`'s own vocabulary (`s1608`, `s4000s`), or `none` |
| `reac.roster.<i>.role` | `auto` \| `master` \| `slave` \| `tap`, as RESOLVED — never as asked |
| `reac.roster.<i>.source` | `autodetected`, or `conf:<file>` naming the file that pinned it |
| `reac.roster.<i>.width` | the published pair's `in/out`, `0/0` where there is no pair |

The node is created once and lives for the process: every change is a property update, so a
client that has found it once never has to find it again. It is a READ surface only — a role
is still written on the segment's own door (`reac.cfg.role`), or pinned across restarts in
the conf.

Environment knobs are a separate surface and live in [ENV-KNOBS.md](ENV-KNOBS.md): they
configure a segment at start-up, where these properties and params drive it while it runs.
