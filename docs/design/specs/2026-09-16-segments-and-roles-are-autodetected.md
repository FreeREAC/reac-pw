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
| §2: the box-master and silent-box elections still hold per segment | `tests/test_reac_hunt_captures.c`, `tests/box-master-slave-join.sh`, `tests/hearing-finds-a-segment.sh` |

## 7. Not proven here

The LIVE trunk. Nothing in this lane ran against `enp131s0.1/.11/.12` with the operator's three
boxes on it: the desk was carrying a show test. What the tests above stand on is veth pairs and
VLAN sub-interfaces inside an unprivileged user+net+pid namespace, which proves the daemon's
decisions and proves nothing about a switch. The rig step is in §5 of the lane's report.
