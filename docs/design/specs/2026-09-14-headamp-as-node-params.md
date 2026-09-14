<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# Head-amp as node parameters — a stagebox preamp app on reac-pw alone

Status: RULED by the operator 2026-09-14; normative for the reac-pw head-amp-door lane, the
openmixer proxy lane and the REAC Stageboxes app

The question this answers: *could a GNOME settings application drive a stagebox's preamps through
libreac, so that installing reac-pw alone is enough to manage every channel — and is that a protocol
reac-pw would have to offer, or one it already offers?*

## 1. What exists today

**The door is already PipeWire-native, and it already works with nothing but reac-pw installed.**

reac-pw's master-role sink node — `reac-playback` (or `reac-playback.<segment>` on a named
segment), `media.class = Audio/Sink` — accepts head-amp changes as `SPA_PARAM_Props`, carried in
the extensible `SPA_PROP_params` bag as alternating (String key, value) pairs:

```
reac.headamp.<wireCh>.phantom = 0 | 1
reac.headamp.<wireCh>.pad     = 0 | 1
reac.headamp.<wireCh>.sens    = 0 .. 0x37      (REAC_HEADAMP_SENS_MAX)
```

`<wireCh>` is the absolute REAC wire channel in decimal, `base + (box_input - 1)`. Values are
accepted as Bool, Int or Float, so a toggle, a spin button and a slider each send their natural
encoding (`src/reac_headamp_prop.c`, `value_pod_to_byte`). The parse is pure and offline-testable;
`src/reac_sink_node.c` `on_param_changed` hands each parsed cell to `reac_pacer_headamp_set`, a
lock-free ring to the RT pacer, which stamps the wire record with libreac's
`reac_ctrl_stamp_headamp`.

The vocabulary is **advertised**, so a client discovers it rather than knowing it: `params[3]` of
the sink's `SPA_PARAM_PropInfo` enumeration names
`reac.headamp.<ch>.<param> | reac.cfg.rate | reac.cfg.role`. `pw-cli enum-params <id> PropInfo`
prints it.

The read side is node properties on the **same** node, so one lookup yields shape and address:

| property | meaning |
|---|---|
| `reac.headamp.channels` | preamp-capable box inputs; `0` until a model is recognised |
| `reac.headamp.caps` | comma-separated tokens, today `phantom,pad,sens` |
| `reac.headamp.base` | the chassis strap, decimal, or `none` |
| `reac.link-state`, `reac.box-model`, `reac.box-width`, `reac.box.mac`, `reac.box-firmware`, `reac.box.reac_version`, `reac.segment` | the box badge, mirrored onto the `reac-capture` node |

So a write from a bare shell is one command:

```
pw-cli set-param <sink-id> Props '{ params = [ "reac.headamp.34.phantom", 1 ] }'
```

**Who may write.** `packaging/reac-pw.service` is a **user** unit: reac-pw runs as the operator on
the operator's PipeWire socket. The only gate is PipeWire session access — no control socket, no
D-Bus name, no polkit, no CLI, no file drop. Any client of that session may write; nothing else
can reach the node at all.

**Where the door is not.** It is not on `reac-capture.<segment>`. The sink node exists **only** in
the master role (`reac_sink_node_new` is never called for a slave), and the master role is the only
role that can put a head-amp record on the wire. Putting the control on the capture node would put
it on a node that exists when we cannot send. The door stays where it is.

## 2. The refusal rules, as the wire states them

- **A box on M offers none.** `mixer-protocol.md` §9: a box in REAC master mode sends no head-amp
  sweep, no identity requests and no group map; its preamps are configured out of band through the
  box's serial port. Head-amp control over a box on M *does not exist on the REAC wire*. Measured:
  a byte-identical SET reached a box on M and the floor did not move (−91.4 dBFS at sens 32, 52 and
  32), against +18.9 dB on an enrolled S-1608 by the same write. reac-pw 0.5.6 therefore publishes
  `reac.headamp.channels = 0` and `reac.headamp.base = none` on a box-master segment, rather than
  publishing a capability the wire cannot carry.
- **No box.** Same empties until a model is recognised.
- **No base.** Without the announced chassis strap there is no wire address; openmixer's actuator
  refuses with `NO_WIRE_ADDRESS` rather than guessing `0`, which would address an S-1608's preamps
  32 slots low and silently.
- **Malformed or out of range.** `reac_headamp_prop_parse` drops the cell: a non-numeric channel, a
  channel past `REAC_HEADAMP_MAX_CH`, an unknown param name, phantom/pad above 1, sens above 0x37.
- **Write-only.** §6: a box never re-broadcasts its head-amp state. There is no readback and
  nothing to query and compare against. The only thing that restores state after a box drops is a
  full re-push at establishment.

**Two gaps, and they are the whole of what is missing.**

1. **A refused write is silent.** A cell dropped by the parse, and a cell written to a node with
   `channels = 0`, both return nothing. The caller sees a successful `set-param`. reac-pw already
   has the pattern for the honest answer one level over: `reac.cfg.rate.state` /
   `reac.cfg.rate.refused` and `reac.cfg.role.state` / `reac.cfg.role.refused` publish an applied /
   pending state and a short refusal code beside the control key. Head-amp has neither.
2. **There is no readback of what reac-pw is asserting.** The box cannot be asked, but reac-pw
   holds the shadow table it re-pushes at every establishment, and publishes none of it. A second
   client — a settings app — cannot render a switch without inventing its own copy.

## 3. The design

### 3a. The door: nothing moves, four properties are added

The head-amp door **is** the parameter interface the question asks for. It needs no rename, no new
transport and no protocol of its own. It needs the properties that make it honest.

**The travel.** `reac.headamp.sens.max` — today `55` (0x37), a flat 1 dB per step under
`dBu = -10 - value + (pad ? 20 : 0)`. Published beside `reac.headamp.caps`, so a client renders the
range it receives and a model with a different travel needs no new client.

**Asserted state.** One node property on the sink, carrying reac-pw's shadow table as a compact
list of cells:

```
reac.headamp.asserted = "34:0=1,34:2=52,35:0=0"        ch:param=value, empty when nothing is set
```

One string, one `pw_stream_update_properties` per change, rather than 144 separate keys churning
the dict on every knob turn. Named `asserted`, not `state`: it is what this daemon is putting on
the wire, never a report from the box, and the name must not let a reader believe otherwise.

**Refusal.** The exact counterparts of the rate and role pair:

```
reac.headamp.state    = "applied" | "unavailable"
reac.headamp.refused  = "none" | "no-box" | "box-master" | "no-base" | "bad-key" | "out-of-range"
```

`box-master` turns today's silence into a sentence a surface can render — *the box is master, its
preamps are preconfigured* — which is the contract of that mode, not an error.

All four ride the existing `sink_publish_link_props` timer and the existing prop composer: no new
thread, no new file, and no change to the record path already byte-verified against a capture.

### 3b. One writer

reac-pw is the only process on the wire and the only holder of the shadow table. Every other
component **observes** these properties and keeps no copy.

What that retires in openmixer, by file:

- `packages/audio-engine/src/reac-head-amp.ts` — the actuator keeps writing the control keys. Its
  `resolveHeadAmpBase` / `noteHeadAmpBase` fallback (§10 of `reac-head-amp-control.md`) exists to
  address a box in M from a remembered base. §9 says those preamps are not addressable over REAC at
  all, so the fallback serves a case the protocol does not have. Question 3 below.
- `packages/server/src/stagebox-registry.ts` — `StageboxEntry.headAmpBase` and `noteHeadAmpBase`,
  the stored counterpart of the same fallback.
- `packages/server/src/stagebox-name-row.ts` — the `headAmpBase` row field and its `derive`.
- `packages/server/src/server.ts:5841` — `entryForKey(key)?.headAmpBase`.

The console's own head-amp rows (`channel-capability-row.ts`, `native-lane-readback.ts`,
`head-amp-defaults-settings.ts`, `head-amp-record-keying.ts`) stay: they are the desk's door for
the desk's strips, and they read the node's published truth. Question 2 below settles whether they
also read `reac.headamp.asserted` instead of holding their own last-written value.

### 3c. The application

**`REAC Stageboxes`** — GTK4 + libadwaita, written in C against libpipewire's registry, matching
reac-pw's own dependency set and the house rule that native audio code is plain C.

- It is a **PipeWire client**, not a REAC one. It links `libpipewire-0.3` and `libspa-0.2`, walks
  the registry for nodes whose `media.class` is `Audio/Sink` and which carry `reac.segment`, and
  binds a `pw_node` proxy per box. It links **no libreac**: every protocol fact it needs — the
  channel count, the capability tokens, the base, the model, the firmware, the REAC version, the
  link state — is already a published property, and the wire record is reac-pw's to build. No
  protocol code, no second copy of the base law, no REST.
- **Sidebar**: one `AdwActionRow` per segment, titled by `reac.box-model` with `reac.box.mac` and
  `reac.box.reac_version` as subtitle, and the link state as the row's badge.
- **Content**: one `AdwExpanderRow` per box input, `1 .. reac.headamp.channels`, built from the
  `reac.headamp.caps` token set — an `AdwSwitchRow` for phantom, one for pad, an `AdwSpinRow` in dB
  for sens. The rows bind to `reac.headamp.asserted`; a write is
  `pw_node_set_param(SPA_PARAM_Props, …)` with one `reac.headamp.<ch>.<param>` cell.
- **When the capability is absent** — `channels = 0`, or `reac.headamp.refused = box-master` — the
  page renders the reason, not a dimmed control that does nothing. A surface never shows a preamp
  control it cannot move.
- **Packaging**: a `reac-pw-stageboxes` subpackage in `packaging/reac-pw.spec`, `Requires: reac-pw`
  plus `gtk4` and `libadwaita`, `BuildRequires: pkgconfig(gtk4) pkgconfig(libadwaita-1)`, shipping
  a `.desktop` file and an AppStream metainfo so GNOME Software lists it. It builds in the same
  meson tree behind an option, so a headless install carries none of it.
- **A gnome-control-center panel is a later step, and an honest one.** GNOME Settings has no
  third-party panel API: a panel means a patched control-center or an upstream proposal. The
  standalone application is the deliverable; the panel is a conversation for after it works.

### 3d. The three lanes, and what each must show to be done

The work splits three ways. Each lane's acceptance line is what it must MEASURE — not what it must
build — and no lane is done on a green suite alone.

**Lane A — the reac-pw head-amp door** (`src/reac_headamp_prop.c`, `src/reac_sink_node.c`):
publishes `reac.headamp.sens.max`, `reac.headamp.asserted`, `reac.headamp.state` and
`reac.headamp.refused`.

*Accepts when:* a `Props` pod carrying `reac.headamp.32.phantom = 1`, driven through
`reac_headamp_prop_parse` → `reac_pacer_headamp_set` → the pacer drain → `reac_ctrl_stamp_headamp`,
emits a frame **byte-identical to libreac's golden** — `tests/ctrl_fixtures.inc`, `FX_L4_HEADAMP`,
a TAG 0x0101 record captured off an M-200 commanding an S-1608
(`m200i-none-48k-clean__m200-s1608-realbox-establish-2026-07-11.pcap`), record bytes
`CH 0x20, PARAM 0x00, VALUE 0x01` — and `reac_ctrl_headamp_record_verify(frame) == 0`. That fixture
is an S-1608 at base 32, so the same assertion pins the base law. A self-consistent check that the
parse returned what the builder built proves nothing; the golden bytes are the oracle.
*And when:* each refusal code is proved by **reading `reac.headamp.refused` back** after the write —
`no-box`, `box-master`, `no-base`, `bad-key`, `out-of-range` — never by counting a round-trip. A
door that refused the write reports a blind pass.
*And when:* after a set, `reac.headamp.asserted` carries that cell, and still does after an
establishment re-push.

**Lane B — the openmixer proxy** (`packages/audio-engine/src/reac-head-amp.ts`,
`packages/server/src/stagebox-registry.ts`, `stagebox-name-row.ts`, `server.ts:5841`): the rows read
`reac.headamp.asserted` as the truth; the remembered-base fallback and `StageboxEntry.headAmpBase`
go.

*Accepts when:* a head-amp value written by any OTHER client — one `pw-cli set-param` from a shell —
appears on the console's row without the console having written it. That is the whole of "proxy",
and a suite that only reads back the console's own write cannot see the difference.
*And when:* on a box in M the row answers the `box-master` code from the node and no write is
attempted, with nothing remembered and nothing guessed.

**Lane C — the REAC Stageboxes app** (its own tree, packaged from `packaging/reac-pw.spec`).

*Accepts when:* on a rig with an enrolled S-1608 and an **enrolled S-1608 only** — no openmixer
running, no console process on the wire — the application lists the box from the graph and a sens
move made in it is **measured on the box's audio**: a tone on box input 1 at a baseline at least
40 dB above the floor, sens 32 → 52, and the measured delta required. A −90 dBFS pair is digital
silence and refutes nothing; prove the injection landed first by moving a control known to be in
that path and requiring the reading to follow it.
*And when:* pad is confirmed as the box's own 20 dB — set sens, engage pad, require the level to
move 20 dB with **no** sens record on the wire.
*And when:* 48 V is confirmed with a meter at the XLR pins. No hardware claim from a switch, a lamp
or a soft meter, ever.

## 4. The rulings (operator, 2026-09-14)

1. **The sens travel — published or assumed?** The law is `dBu = -10 - value + (pad ? 20 : 0)`,
   0x00..0x37, a flat 1 dB per step, and it holds on every model decoded so far.
   **ANSWERED: publish the travel as a property — `reac.headamp.sens.max`.** The application renders
   the travel it receives; a model with a different range needs no new client.
2. **Do the console's head-amp rows retire or proxy?**
   **ANSWERED: the console's head-amp rows PROXY the node's truth.** openmixer keeps its rows — they
   are the desk's door for the desk's strips and carry the capability gating and the defaults — and
   they read `reac.headamp.asserted` instead of echoing the last value they wrote. One writer, one
   store, two surfaces.
3. **Does the remembered-base fallback retire?** It was built so a box that had once announced
   base 32 could still be addressed after switching to M. §9 says a box on M has no head-amp on the
   wire in either direction.
   **ANSWERED: retire it.** The fallback in `reac-head-amp.ts` and the `headAmpBase` field in the
   registry go; the refusal stays, and the surface says `box-master` rather than writing into
   silence.
4. **Who may write?** reac-pw is a user unit on the user's PipeWire socket, so the answer is "anyone
   in the operator's session", the same answer as for volume on any device.
   **ANSWERED: no polkit. PipeWire session access is the boundary.** A permission model becomes
   meaningful only if reac-pw ever runs as a system service, and that is a separate decision.
5. **The readback shape.** One compact `reac.headamp.asserted` cell list, or 144 individual
   `reac.headamp.<ch>.<param>` properties?
   **ANSWERED: one compact asserted-cells property, `reac.headamp.asserted`.** Per-cell properties
   would put the control keys and the readback keys in the same namespace under the same names — the
   one confusion the current header is careful to avoid — and would churn the property dict on every
   knob turn.
