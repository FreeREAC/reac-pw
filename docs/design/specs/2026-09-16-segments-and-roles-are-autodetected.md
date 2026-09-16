<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# Segments and roles are AUTODETECTED — the one override is a file, never an env key

Status: RULED by the operator 2026-09-16, verbatim — *"We must never depend on the env vars of
the daemon, all has to be autodetected unless you have an explicit configuration file that
overrides it. We should detect vlans, masters and slaves and connect everything."* Normative for
this daemon's segment discovery and role election. SUPERSEDES, for ROLE and for SEGMENT
DECLARATION only: openmixer's `2026-08-23-reac-trunk-vlan-daemon.md` amendment 2026-09-02 rules
(d) and (e) (`REAC_ROLE_<segment>` as the per-segment role layer, and the console projecting one
key per discovered segment) and §4 of this repo's
[`2026-09-16-auto-role-per-segment.md`](2026-09-16-auto-role-per-segment.md) ("what the console
must write"). Everything else in both — the hunt's four cases, the tap ruling, the link budget,
the hold-down, the mint alias, the layered lookup for RATE and the other host-wide keys — stands
unchanged.

## 0. The live case this answers

2026-09-16. The rig moved from one box on a 100 Mbit USB NIC to three boxes on a 1 Gbit trunk
(`enp131s0.1`, `.11`, `.12`; `.13` empty). The console's generated
`~/.config/reac-pw/reac-pw.env` still carried the previous rig's answers, and those answers were
now wrong: the VLAN segments were pinned `tap`, so the daemon served three mirror ports on a
fabric that had no mirror, transmitted nothing, and enrolled no box. Every segment was up, every
node was on the graph, and no audio moved.

**Nothing observed the wire.** A launch-time key is a decision taken before there is anything to
decide against, and it survives every change to the thing it describes. The previous pass
(`2026-09-16-auto-role-per-segment.md` §0) fixed the *value* the console writes; this one removes
the *dependency*. The env file is a rig-shaped artefact that goes stale silently, and a stale file
and a correct one are indistinguishable from inside the daemon.

## 1. RULING — segment discovery is the daemon's, entirely

The daemon derives its segment set from the host and the wire, and from nothing that was typed:

- **every UP Ethernet interface** that is not loopback and not wireless is sniffed
  (`reac_ifscan`, link is the gate to listen), and becomes a SEGMENT on the first frame that
  classifies as REAC (hearing is the gate to serve);
- **every VLAN sub-interface of such an interface is an interface**, so it reaches the same table
  by the same rule — a host that already carries `enp131s0.11` needs nobody to mention it;
- **a tagged REAC frame heard on a trunk for a VLAN id with NO sub-interface is REPORTED by its
  id**, and the sub-interface is created if the daemon can do it without privilege escalation
  (`CAP_NET_ADMIN`, granted 2026-08-23, trunk spec §4c). Where it cannot, the refusal NAMES the
  VLAN id and prints the `ip link` command that would fix it — an absence is never silent
  (trunk spec §4e);
- **management-only interfaces are left alone.** A NIC with link that never carries a REAC frame
  is never a segment, never a node, never a row. It costs one idle passive socket and nothing
  else. This is already true and is restated because it is what makes rule 1 safe;
- **hot-plug is the same path.** An interface or a VLAN that appears or disappears while the
  daemon runs adds or removes its segment, over rtnetlink, with no restart — the hold-down and
  the flap counter of the 2026-09-02 amendment (b) unchanged.

## 2. RULING — the default role of every segment is AUTO, and nothing can make it otherwise

`auto` is not a value the configuration happens to carry; it is what a segment IS before anything
overrides it. With no configuration of any kind the daemon elects per segment from the wire:
box-master → we SLAVE-join it; foreign desk → we TAP; silent → we MASTER and flood
(`reac_hunt`, `reac_knock`, and the link-budget admission that refuses the second master on one
physical port).

**`REAC_ROLE` and `REAC_ROLE_<segment>` are RETIRED as role sources**, in every layer — the
process environment, `reac-pw.env` and `reac.env` alike. A key that is still present is not
obeyed and is not silently dropped either: it is NAMED at start as ignored, with this spec's
reason, so a host carrying a stale console-generated file is told why its pins stopped applying.
`--role` on the command line keeps its meaning as the dev/`--live` affordance; argv is a decision
taken for one invocation, which is the one thing a stale file can never be.

**`REAC_ROLE_<segment>` is also retired as a segment DECLARATION.** `reac_declared_vlan` read the
KEY's name to mint a VLAN before anything was heard (the cold-boot fault of 2026-09-15, which is
real and is kept); the declaration now comes from §3's file, where naming a segment is an explicit
act rather than a side effect of a role projection. This is the mechanism that put three master
VLANs on a 100 Mbit port with no box on any of them (auto-role §5d) — the declaration outlived
what declared it.

## 3. RULING — the ONE override is `~/.config/reac-pw/reac-pw.conf`

One file, hand-written, **never generated by anything**. The console does not write it: a
generated file cannot be an operator's override, because the generator's idea of the rig is
exactly the thing that goes stale (one store, one writer).

### 3a. The grammar, exactly

INI-like. A `#` or `;` in column one, or after whitespace, starts a comment; blank lines are
ignored; keys and values are trimmed; values are unquoted or single/double quoted; **keys, section
keywords and VALUES are case-insensitive, segment names are NOT** (an interface name is
case-sensitive to the kernel; the values fold because this is a file a human types, and `TAP`
meaning something other than `tap` would be a trap with no upside).

```ini
# ~/.config/reac-pw/reac-pw.conf — the operator's overrides. reac-pw never writes this file.

[segment enp131s0.11]
role = tap          # auto | master | slave | tap   (default: auto)

[segment enp131s0.13]
ignore = yes        # yes|no, true|false, 1|0       (default: no)

[segment enp131s0.12]
role = auto         # legal, and exactly the same as saying nothing
```

- **`[segment <name>]`** — `<name>` is the interface name, which §5 of the trunk spec already
  rules IS the segment's identity. `[segment enp131s0.11]` names a VLAN sub-interface, and by
  naming it DECLARES it: it is minted at start and whenever its parent appears, whether or not
  anything has ever been heard on it (§2, and `reac_declared_vlan.h`'s cold-boot reason).
- **`role`** — the intent vocabulary `reac_role.h` already defines. It is the only thing that can
  pin a segment. `tap` remains a per-segment fact only; there is no host-wide role.
- **`ignore`** — the segment is never sniffed, never served, never minted, and its netdev is left
  exactly as found. This is the `REAC_IFACES_DENY` of trunk spec §8, finally built, in the place
  the ruling puts it.
- **Anything else is REPORTED BY NAME and does not stop the daemon**: an unknown section keyword,
  an unknown key, an unparsable `role` value, a duplicate section. A typo in this file must be
  visible and must not take a desk down mid-show, and those two requirements are both satisfiable
  at once only by a refusal that is loud and local. A file that does not exist is not an error and
  is not a warning: it is the normal case.

### 3b. What the env file keeps

`reac-pw.env` and `reac.env` stay exactly as they are for the HOST-WIDE keys that are not about
which end of a wire we are — `REAC_RATE` (and `REAC_RATE_<segment>`), `REAC_MIXER`, `REAC_NAME`,
`REAC_HEADAMP`, `REAC_TX`, `REAC_SRC_MAC`, `REAC_BOX_CHANNELS` — read through `reac_conf`'s
declared precedence, unchanged. Only role and declaration move. The precedence with the new file
in it, highest first: **argv → `reac-pw.conf` → process environment → `reac-pw.env` →
`reac.env` → built-in**, and for role only the first two can answer at all.

### 3c. When the file is read

**Whenever a segment's role is RESOLVED** — its sniffer opens, its listener opens, it is asked
whether it is ignored — guarded by one `stat()`, so a reload happens only when the file's mtime,
size or inode moved, or it appeared or vanished. That is the same lifetime every other layer
`reac_conf` reads already has (it opens its files on every lookup), and a new file with a
different one is an inconsistency nobody remembers at 2 a.m.

**This is not a live role change.** A segment already running keeps the engine it opened with; a
re-read decides what the NEXT resolution sees — a link-up, a hot-plug, a re-link. The live path
stays `reac.cfg.role` on the segment's own door, and the SIGHUP re-election of
`2026-09-16-auto-role-per-segment.md` §5b, which this does not replace and does not build.

## 4. RULING — the daemon SAYS what it detected and what the file overrode

At start, after the conf is read and before any listener opens, one block on stderr: the conf's
path and whether it exists, every override it carries (segment, key, value), and every line it
refused. Then, per segment, the line that serves it names its role AND where that role came
from — `autodetected` or `reac-pw.conf`. A configuration whose effect cannot be read back is a
configuration nobody can debug, and this rig has spent a night on exactly that.

## 5. What the console must do (the other half)

openmixer **stops writing `REAC_ROLE_<segment>` altogether** — including `=auto`, which is now
what a segment is with nothing said. Its role intent reaches the running daemon the way every
other live control does: `reac.cfg.role` on the segment's own door, query-and-compare against the
published `reac.role` (auto-role §5b). An operator who wants a role to SURVIVE a restart writes
`reac-pw.conf` by hand; the console may offer to show it, and must never write it.

The generated-marker rule and `reac:config-not-generated` still apply to `reac-pw.env` for the
host-wide keys the console does own.

## 6. Proven, and by what

| rule | test |
|---|---|
| §1: no conf and no env → a trunk with two VLAN sub-interfaces yields three segments, all `auto` | `tests/segments-autodetect.sh` arm A |
| §2: a stale `REAC_ROLE_<segment>` pin is IGNORED and named | `tests/segments-autodetect.sh` arm A, `tests/test_reac_segconf.c` |
| §3a: `role = tap` overrides one segment; `ignore` removes one | `tests/segments-autodetect.sh` arms B/C, `tests/test_reac_segconf.c` |
| §3a: the grammar — comments, quotes, case, refusals by name | `tests/test_reac_segconf.c` |
| §1: a tagged VID with no sub-interface is announced by its id | `tests/segments-autodetect.sh` arm D |
| §1: hot-plug adds a segment with no restart | `tests/segments-autodetect.sh` arm E |
| §3c: a conf written while the daemon runs is seen at the next resolution | `tests/hearing-finds-a-segment.sh` (the pin phase) |
| amendment: no recognised box, no node; a box arriving and leaving | `tests/no-box-no-node.sh`, `tests/pins-need-no-door.sh` |
| §2: the box-master and silent-box elections still hold per segment | `tests/test_reac_hunt_captures.c`, `tests/box-master-slave-join.sh`, `tests/hearing-finds-a-segment.sh` |

## 7. Not proven here

The LIVE trunk. Nothing in this lane ran against `enp131s0.1/.11/.12` with the operator's three
boxes on it: the desk was carrying a show test. What the tests above stand on is veth pairs and
VLAN sub-interfaces inside an unprivileged user+net+pid namespace, which proves the daemon's
decisions and proves nothing about a switch. The rig step is in §5 of the lane's report.

## Amendment 2026-09-16 (later) — no recognised box, no node on the graph

**RULED by the operator, same day:** *"a segment with NO recognised box must not appear in the
PipeWire graph at all. The reac-capture/reac-playback nodes for a segment are created when a box
is recognised on that wire and torn down when it leaves; the probing state lives in the daemon's
own row/log, not on the graph."*

**The live case.** The empty untagged trunk segment `enp131s0.1` — probing, box-model `none` —
logged *"MASTER autodetect — the segment's door is on the graph now (reac-playback, no ports
yet)"*, and the console rendered a device reading `none / 0 in`. A row for a thing that is not
there, which the operator must then learn to ignore.

**This SUPERSEDES the "door before the wire" half of Q5 option C** (openmixer's
`2026-09-13-reac-plug-and-play.md` §0/§4 and `2026-08-20-reac-master-arbitration.md`'s Q5 answer,
as built in reac-pw 0.5.1 and tested by `tests/doors-open-before-the-wire.sh`). The reason that
ruling gave — a role, tap included, must be settable before anything enrols — is a real
requirement and is NOT withdrawn; what is withdrawn is satisfying it with a **zero-port PipeWire
node**. A role is settable before anything enrols through `reac-pw.conf` (§3) and, live, through
the daemon's own door on a segment that HAS one.

### The mechanical rule

**A segment publishes its nodes only when it has SOMETHING TO CARRY** — a box recognised on
the wire, a master's stream to tap, or a width the operator pinned. Not "when it has ports":
the empty trunk segment's door on the desk carried four, so the port count is a symptom of
this defect and not a test for it. Four cases, and they are one rule:

| the wire | ports | on the graph |
|---|---|---|
| a box recognised on it (master enrolled it, or we slave-joined a box master) | its declared width | **yes** — `reac-capture` in / `reac-playback` out, sized and labelled by the box |
| a foreign MASTER heard and tapped | the master's downstream width | **yes** — the tap serves a real stream, and this rule is not about who owns the wire |
| nothing recognised: probing, refused, or a tap with no stream | 0 | **no node at all** |
| `--box MODEL` pinned (a fixed installation) | the pinned width | **yes** — the pin is an explicit declaration that this box belongs on this wire, the same class of act as a `reac-pw.conf` section, and its whole purpose is that a patch survives a box that is not powered yet |

**And it is symmetric: a box that LEAVES takes its nodes with it.** The master's own FSM already
clears `recognized_box` when the peer goes (`reac_pacer`, `reac_master_forget_box`); the
autodetect watcher acts on that NULL and destroys both nodes. A node that outlives its box is the
same `none / 0 in` row arriving by the other door.

### What this costs, said before it is deployed

**openmixer's `/reac/segment` roster is a GRAPH SCAN** (`packages/server/src/reac-props-discovery.ts`,
governed by `2026-08-20-reac-auto-spine.md`): a segment is discovered by reading `reac.segment`
off a node's props, and by nothing else. So a segment with no node has **no row on the console at
all** — not a row that says "probing". That is a real loss and it is the console's half to fix:
the daemon must offer its segment roster somewhere that is not a node's props, and the console
must read it there. **Not built in this lane, and named rather than glossed.** Until it is, an
empty segment is visible in the daemon's journal and nowhere else — which is what the ruling
asks for and is less wrong than a device that says `none / 0 in`.

The console already carries a rule for exactly this shape ("a reac-pw ghost door, box-model none,
width 0x0, never outranks the real box's door") — that rule becomes dead code once no such door
is ever published, and should be retired with the retargeting rather than left as a guard against
a thing that cannot happen.

### Proven by

`tests/no-box-no-node.sh`: an empty segment yields ZERO graph nodes for its name (with a
recognised box on another arm as the positive control, because an absence measured by an
instrument that has never seen a presence is not a measurement), a box that enrols yields the
sized pair, and a box that goes takes them away again. The test asserts NODES, not ports: a
private PipeWire with no session manager reports 0 ports on a healthy enrolled segment for as
long as it lives, which is a fact about the fixture and not about the daemon.

## Amendment 2026-09-16 (third) — the drop-in directory, and the roster the console reads

**RULED by the operator, same day, two sentences.** *"Expert users can configure reac-pw OR OMX
to override it"* — so the console needs a door of its own that is not the operator's file. *"Not
autodetecting is an error"* — so the console must still SEE a segment that is probing, which the
previous amendment took off the graph and left in this daemon's journal ("a real loss and it is
the console's half to fix", above, verbatim). Both halves are built here.

### A. `reac-pw.conf.d/*.conf` — the second door, same grammar

The daemon reads, in this order:

1. `~/.config/reac-pw/reac-pw.conf` — the hand-written file of §3, unchanged, still never
   generated by anything;
2. then every `*.conf` in `~/.config/reac-pw/reac-pw.conf.d/`, **in byte order of the file name**
   (`strcmp`, not locale collation — a config order that changes with `LANG` is not an order).

**The grammar is §3a's, exactly** — the same `[segment <name>]` sections, the same `role` and
`ignore` keys, the same case rules, the same refusals. One parser, one vocabulary; a second
grammar for the same fact would be the divergence this spec family exists to remove.

**LATER WINS, PER KEY.** A file read later overrides the same key of the same segment; keys it
does not mention keep whatever the earlier file said. A repeated `[segment X]` header ACROSS files
is an override and is silent; a repeated header WITHIN one file is still refused by name (§3a),
because there it is a typo and not an intent.

**So a drop-in overrides the hand-written file, and that is the ruling, not an accident.** Four
reasons, stated because the reverse order is the obvious alternative:

1. it is the drop-in convention this host already runs on — systemd units, `sysctl.d`, `udev`,
   WirePlumber — where the base file is the default and the drop-in is the later word. A
   precedence an operator has to be told is a precedence they will get wrong at 2 a.m.;
2. the operator ruled that **either** door may override; a console whose file can never win is
   not a door, it is a suggestion;
3. the operator keeps the last word inside the same grammar, by a name: a `99-local.conf` sorts
   after `50-openmixer.conf`, and deleting the console's file is always available. That is a
   precedence you can READ OFF THE DIRECTORY, which is the property the hand-written-wins order
   does not have;
4. it is safe only because of §C below: every key names the file that set it, at start and on the
   roster. The 2026-09-16 fault was not that a generated file existed — it was that nothing could
   see which answer came from where.

**The console's file is `reac-pw.conf.d/50-openmixer.conf` and nothing else.** openmixer never
touches `reac-pw.conf`; §5's rule (the console writes no role) is amended only to this extent —
an explicit, operator-initiated override is written to its OWN file, carries the
generated-marker, and is rewritten whole. One store, one writer, per file.

**No system directory.** `/etc/reac-pw/reac-pw.conf.d/` is NOT read, for the reason the package
already states (`packaging/reac-pw.spec`): nothing this daemon reads is host-wide system config —
every layer `reac_conf.h` declares is per-user, and the daemon is a USER unit that needs `$HOME`
to find its PipeWire socket at all. A system layer would be a file no running instance is
guaranteed to be able to read.

**Re-read: the same one-`stat` rule as §3c**, extended by the one thing a directory adds. On each
resolution: `stat` the conf, `stat` the directory, and — only when neither moved — `stat` each
drop-in that was read. A reload happens when any mtime, size or inode moved, or a file or the
directory appeared or vanished. A file added or removed moves the directory's own mtime, so the
common case costs two stats and the loaded case a handful; no file is opened unless something
moved.

**Every refusal names the FILE and the line**, `reac-pw.conf.d/50-openmixer.conf:7: ...`, not a
bare line number: with N files a line number alone points at nothing. An unreadable drop-in, and a
directory with more files than the bound, are refusals of the same kind — reported by name, never
silent, never fatal.

### B. The ROSTER node — one node, no ports, every segment on it

**The daemon publishes ONE persistent PipeWire node, `reac-pw`**, from start to exit, whatever the
wire holds. It has **no ports** and a `media.class` no session manager knows (`Reac/Roster`), so
nothing links it, routes it, or offers it as a device; it carries `reac.roster = 1` so a client
finds it by a property and not by a name. It is not a segment's door and never becomes one: the
per-box `reac.segment` props on `reac-capture` / `reac-playback` are unchanged, and the console's
existing discovery (`packages/server/src/reac-props-discovery.ts`) keeps reading them exactly as
it does today.

**Its props ARE the roster**, one group per segment the daemon runs, index-keyed:

```
reac.roster            = 1
reac.roster.n          = 3
reac.roster.0.name     = enp131s0.11         # the segment's identity: the interface name
reac.roster.0.state    = probing | established | slave | tap | refused | ignored
reac.roster.0.model    = s1608               # `none` when nothing is recognised
reac.roster.0.role     = auto | master | slave | tap     # as RESOLVED, not as asked
reac.roster.0.source   = autodetected | conf:<file>      # conf:reac-pw.conf.d/50-openmixer.conf
reac.roster.0.width    = 16/8                # in/out; `0/0` when there is nothing to carry
reac.roster.1.name     = ...
```

- **the index is an ORDER, not an identity.** Segments are listed in byte order of their name, so
  the order is stable and derived from nothing stored; a reader keys off `.name`. `reac.roster.n`
  is what bounds the walk, and the groups past it are REMOVED from the props, not blanked — an
  empty string and an absent key must not read alike;
- **`state` is the segment's, and it is derived every tick from the tables that already hold it**
  — `ignored` from the conf, `tap`/`slave`/`refused` from the listener's own engine, `established`
  when a box is recognised on the wire, `probing` for everything the daemon is sniffing and has
  not decided. No second ledger: nothing writes a roster field, the roster is READ off the
  daemon's state (derive, never store the derivation);
- **`model` is `reac.box-model`'s own vocabulary** — the token (`s1608`, `s4000s`) and
  `none`, exactly as the segment's own node publishes it. A roster that spelled a box a
  second way would be a second vocabulary for one fact, and the display string carries the
  width that `.width` already owns;
- **`width` is the published pair's width** — the same numbers the `autodetected ... ->
  reac-capture N in / reac-playback M out` line prints — and `0/0` exactly when there is no pair;
- **a change updates the PROPS, never the node.** The node is created once and lives for the
  process; each tick computes the wanted roster, diffs it against what was published, and updates
  only the keys that moved (an empty diff updates nothing). A node id that churns is a client's
  discovery storm, and this node exists to stop one.

**What this restores.** An empty segment — no box, no node, exactly as the second amendment
rules — is now VISIBLE: `state=probing`, `model=none`, `width=0/0`, on a node that is always
there. "Not autodetecting is an error" is a thing the console can now say, because it can see a
segment that is probing and never establishes. The second amendment's named loss is closed, and
its own words ("the daemon must offer its segment roster somewhere that is not a node's props")
are met in the only way that keeps ONE graph client: a node's props, on a node that is not a
segment.

### C. Provenance, everywhere a role is answered

Every place the daemon reports where a role came from — the start block of §4, the per-segment
serve line, and `reac.roster.<i>.source` — names the FILE, not the layer:
`autodetected`, `conf:reac-pw.conf`, or `conf:reac-pw.conf.d/50-openmixer.conf`. §4's block gains
the list of files read, in read order, and says which of them set each override. This is what
makes A's last-wins order debuggable rather than merely defined.

### Proven, and by what

| rule | test |
|---|---|
| A: read order, last-wins per key, a cross-file repeat is an override | `tests/test_reac_segconf.c` |
| A: refusals name file AND line; an unreadable drop-in is refused, not fatal | `tests/test_reac_segconf.c` |
| A: a drop-in overrides the hand-written file on the real binary | `tests/roster-node-lists-every-segment.sh` arm B |
| B: one roster node, no ports, with one empty and one boxed segment on it | `tests/roster-node-lists-every-segment.sh` arms A/B |
| B: a state change updates the props and NOT the node id | `tests/roster-node-lists-every-segment.sh` arm C |
| B: the diff is empty when nothing moved; a removed group's keys are removed | `tests/test_reac_roster.c` |
| B: the prop grammar — index keys, `none`, `0/0`, the state vocabulary | `tests/test_reac_roster.c` |

### Not proven here

The console's half: openmixer does not read `reac.roster.*` yet, and does not write
`50-openmixer.conf` yet. Both are named in this spec so the door and its reader are one design,
and neither is built in this lane. And, as in §7, nothing here ran against the operator's live
trunk — the boxes were carrying a show test.
