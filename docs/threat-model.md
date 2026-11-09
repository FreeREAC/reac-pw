<!-- SPDX-License-Identifier: GPL-3.0-or-later
     Copyright (C) 2026 Pau Aliagas -->
# Threat model

## 1. What it is and where it runs

reac-pw is a `libpipewire-0.3` client daemon (`/usr/bin/reac-pw`) that puts Roland REAC
stageboxes into the PipeWire graph as nodes. With no `--live`/`--pcap` it listens for REAC
traffic (EtherType `0x8819`) on every linked Ethernet interface and opens one capture and
one playback node per segment. The REAC byte layout, the control plane, the sockets and the
pacer live in libreac and libreac-transport; reac-pw links them (`README.md`).

- **User, not root.** It runs as a systemd *user* unit (`packaging/reac-pw.service`) on the
  user's PipeWire socket. `refuse_if_root()` in `src/main.c` exits when the effective uid is
  0 outside a mapped user namespace.
- **File capabilities.** The RPM sets `cap_net_raw,cap_net_admin,cap_sys_nice=ep` on the
  binary (`packaging/reac-pw.spec`): raw `0x8819` capture and send; VLAN sub-interfaces,
  `SO_TXTIME` and the etf qdisc it installs over rtnetlink; and `SCHED_FIFO`. At start
  `src/main.c` reads `CapEff` from `/proc/self/status`: no `CAP_NET_RAW` is fatal, no
  `CAP_NET_ADMIN` is logged and VLAN work is refused.
- **Real-time threads.** A `SCHED_FIFO` cadence pacer (master) and an upstream engine (slave)
  are started through libreac-transport; `REACPW_RT_PRIO` sets the priority, and the unit sets
  `LimitRTPRIO=95` and `LimitMEMLOCK=infinity`. reac-pw's own `src/` never raises a scheduling
  class itself (`tests/rt-prio-single-door.sh`).

## 2. Inputs

| input | read by | who can write it | trusted |
|---|---|---|---|
| REAC frames (`0x8819`) on any linked interface: audio, control blocks, identity pages, discovery | libreac / libreac-transport (rx feeder, `reac_topo_tap_read`, `reac_ctrl_parse` in `src/reac_knock_wire.c`) | anyone on the LAN segment or VLAN | **no** |
| `SPA_PARAM_Props` writes: `reac.headamp.<ch>.<param>`, `reac.cfg.rate`, `reac.cfg.role` | `src/reac_headamp_prop.c`, `src/reac_rate_cfg.c`, `src/reac_role_cfg.c` via `src/reac_sink_node.c`, `src/reac_source_node.c` | any client of the user's PipeWire session | **no** |
| rtnetlink replies (RTM_GETLINK alias and link kind; address, link and route dumps) | `src/reac_link_alias.c`, `src/reac_served_mark.c`, `src/reac_link_owner.c` | the kernel; netdev names and aliases can be set by anyone holding `CAP_NET_ADMIN` | partly: the framing is the kernel's, the alias and name text are not ours |
| `~/.config/reac-pw/reac-pw.conf` and `reac-pw.conf.d/*.conf` | `src/reac_segconf.c`, `src/reac_declared_vlan.c` | the local user (the file is in `$HOME`) | treated as untrusted text, parsed with refusals |
| `~/.config/reac-pw/reac-pw.env` (`REAC_<KEY>[_<segment>]`) | libreac-transport's `reac_conf_lookup`, through `src/reac_knobs.c` and `src/main.c` | the local user | yes (user's own configuration) |
| environment variables (`REAC_*`, `REACPW_*`, `HOME`) | `src/reac_knobs.c`, `src/reac_segconf.c` | the local user / the user's service manager | yes |
| command line: `--box MODEL[:LABEL]`, `--live`, `--tx`, `--rate`, `--role`, `--src-mac`, `--headamp`, `--set KEY=VALUE`, ... | `src/main.c`, `src/reac_box_pin.c`, `src/reac_knobs.c` | the local user who starts it | yes |
| `--pcap FILE` (offline replay) | libreac-transport's pcap source | whoever supplied the file | **no** |
| `/proc/self/status`, `/proc/self/uid_map`, `/proc/net/unix` | `src/main.c` | the kernel | yes |
| the box-model table | libreac (compiled in) | the packager | yes |

## 3. Trust boundary

Two boundaries matter. The network one: anything that can put an Ethernet frame on a segment
reac-pw listens on reaches parsers in a process holding `CAP_NET_RAW` and `CAP_NET_ADMIN`
and running `SCHED_FIFO` threads; REAC carries no authentication, so a forged master, box
or control block is indistinguishable from a real one at the protocol level. The local one:
there is no control socket and no D-Bus name (`docs/NODE-PROPERTIES.md`); the only gate on
writes is PipeWire session access, so every client of the user's session can switch phantom
power, change the rate or swap the role. Nothing narrower than the session is enforced.
The user's own files, environment and command line are inside the boundary.

## 4. What the code assumes and enforces

- **Head-amp writes** (`src/reac_headamp_prop.c`): the key must be `reac.headamp.`, a decimal
  channel, `.`, and `phantom`/`pad`/`sens` or it is `bad-key`; a channel outside
  `0..REAC_HEADAMP_MAX_CH-1` or a value outside `0..1` (phantom, pad) or
  `0..REAC_HEADAMP_SENS_MAX` (sens) is `out-of-range`. The parser never writes past the cell
  capacity it is given. A refused cell reaches no send table. Refusals are published on
  `reac.headamp.refused`.
- **Rate writes** (`src/reac_rate_cfg.c`): the first `reac.cfg.rate` value must be an Int or a
  Float with a nearest integer in `int` range; `reac_rate_cfg_decide` refuses a slave, any rate
  off the closed list 44100/48000/96000, and any rate the segment cannot drive, each with its
  published code on `reac.cfg.rate.refused`.
- **Role writes** (`src/reac_role_cfg.c`): only `0` (master) and `1` (slave); anything else is
  malformed and leaves the role as it was; the answer is on `reac.cfg.role.refused`/`.state`.
- **Configuration file** (`src/reac_segconf.c`, `src/reac_segconf.h`): each file is read up to
  64 KiB - 1; lines of 512 bytes or more are refused whole; at most `REAC_SEGCONF_MAX` (32)
  segments and `REAC_SEGCONF_FILES` (17) files, overflow counted and reported; section names
  must be shorter than `IFNAMSIZ`; unknown keys and any `knock` key are refused; a model on a
  non-box segment is dropped. Refusals are counted and the first `REAC_SEGCONF_REFUSALS` (8)
  kept with `file:line`. Declared VLANs (`src/reac_declared_vlan.c`) need a vid of 1..4094
  and a parent of `[A-Za-z0-9.:_-]`.
- **`--box` pin** (`src/reac_box_pin.c`): the text before the first `:` must be a whole token
  of the box-model table; NULL, empty or unknown is refused.
- **rtnetlink** (`src/reac_link_alias.c`, `src/reac_link_owner.c`): replies are walked with
  `NLMSG_OK`/`NLMSG_NEXT` and `RTA_OK`/`RTA_NEXT` over a fixed buffer; attributes are copied
  only when their payload is large enough; alias and kind strings are cut to the buffer and
  NUL-terminated; an alias too long for the kernel is refused before it is written; sockets
  are `SOCK_CLOEXEC` and closed on every path.
- **Knobs** (`src/reac_knobs.c`): at most `REAC_KNOBS_ARGV_MAX` (32) `--set` keys, each value
  cut to 256 bytes.
- **Process**: refuses to run as root (`src/main.c`, `tests/refuses-root.sh`); exits without
  `CAP_NET_RAW`.

The receive gate (`src/reac_rx_bind.c`) binds a non-master segment to one peer MAC: the one
the hunt decided on, or a master MAC that has held for `REAC_RX_BIND_FRAMES` frames and
`REAC_RX_BIND_STABLE_NS`, so one forged frame does not move it. That is a source-MAC check,
not authentication: a sender that uses the bound MAC is accepted.

Not enforced by reac-pw: any authentication of REAC frames; any rate limit on Props writes;
any per-client authorisation inside the PipeWire session.

## 5. Fuzz coverage

Harnesses are in `fuzz/` (libFuzzer, ASan and UBSan; `fuzz/README.md`), run in CI by
`.github/workflows/fuzz.yml`.

| untrusted input | harness |
|---|---|
| `reac.headamp.*` Props write | `fuzz_headamp` |
| `reac.cfg.rate` Props write and its decision | `fuzz_rate` |
| `reac.cfg.role` Props write and its answer | `fuzz_role` |
| `reac-pw.conf` and drop-ins, declared VLANs (text parser) | `fuzz_segconf` |
| `--box MODEL[:LABEL]` | `fuzz_pin` |
| RTM_GETLINK / RTM_NEWLINK alias and kind, served-mark policy | `fuzz_link` |
| etf catch-up arithmetic (lead and frame rate) | `fuzz_qdisc` |

### Gaps

- **REAC frames from the LAN have no harness in this repository.** Frame, control-block,
  identity-page, discovery and topology-tap parsing are libreac's and libreac-transport's;
  `fuzz/README.md` leaves them to those libraries' own harnesses. reac-pw's use of the parsed
  results (`src/reac_rx_bind.c`, `src/reac_knock_wire.c`, `src/reac_box_row.c`) is covered by
  unit tests only.
- **`--pcap` files**: parsed by libreac-transport; no harness here.
- **rtnetlink dumps in `src/reac_link_owner.c`** (address, link and route dumps behind
  `reac_link_owner_read`): not driven by `fuzz_link` or any other harness.
- **File loader of `src/reac_segconf.c`** (`reac_segconf_load`, the drop-in directory walk):
  not fuzzed; only the text parser is.
- **Command-line options other than `--box`** and the `.env`/environment knob values: parsed
  in `src/main.c` and `src/reac_knobs.c` with no harness (inside the trust boundary).
- **PipeWire node glue** (`src/reac_sink_node.c`, `src/reac_source_node.c`): only the three
  Props parsers they call are fuzzed; the glue is exercised against a live graph in `tests/`.
- **Properties not checked**: no harness checks behaviour under a hostile peer on the wire
  (forged master, flooding, MAC spoofing), nor the effect of repeated Props writes from a
  session client.

## 6. Reporting

Report a vulnerability privately through GitHub: the repository's Security tab, "Report a
vulnerability". The policy is the organisation's `SECURITY.md`.
