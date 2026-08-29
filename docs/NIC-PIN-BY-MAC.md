# Pinning the REAC NIC by MAC, not by name — a proposal (not implemented)

> **CORRECTED 2026-08-29 — MAC pinning would NOT have caught the fault below.**
> This note was written against a NIC that came back under a *different* name.
> On 2026-08-29 the AX88179 carrying the S-0808 came back under the **same** name
> and the **same MAC**, with a **new ifindex**. Resolving a MAC to a name would
> have returned the same name it already had, and the "if the name changed" test
> proposed under *Resolution timing* below would not have fired. What actually
> breaks is the **binding**: an `AF_PACKET` socket holds an **ifindex**, and no
> name- or MAC-based check observes that. The fix that landed
> (`fix/the-binding-is-an-ifindex`) compares the socket's own bound ifindex
> against what the name resolves to now, and **exits** so the unit restarts.
> Pinning by MAC remains worth doing — it removes a *different* failure, the
> rename across a reboot — but it is not a substitute for the index check, and
> nothing below should be read as covering re-enumeration.

`--live IFNAME` and `--tx IFNAME` name the REAC NIC by its **kernel-assigned
interface name** (`enp131s0`, `eth0`, ...). That name is not a property of the
NIC — it is assigned by udev's persistent-naming rules from bus topology
(`enp<bus>s<slot>...`) or enumeration order (`eth0`, `eth1`, ...), and both of
those can change under the operator with the physical NIC untouched:

- a USB NIC moved to a different port, or re-enumerated after a replug, gets a
  **different** `enp*` name (the exact failure that motivated this note: a
  rig's REAC NIC was `eth0` in one boot and `enp128s20f0u6` in the next);
- a PCI NIC can renumber across a kernel/BIOS change that alters bus
  enumeration order;
- a fresh install or a distro migration can pick different naming-scheme
  defaults entirely.

Whatever named the NIC when a systemd unit or `rig.env` was last written can
stop naming it at any later boot, and `reac_capture_open()`'s
`SIOCGIFINDEX` on the stale name fails exactly like a NIC that was never
there — "no such interface", not "the interface you meant moved". Companion
fix (`fix/loud-on-vanished-interface`, this branch) makes that failure loud
instead of silent; it does not make the *name* stable.

## The proposal

Let the operator pin the REAC NIC by its **hardware (MAC) address** instead of
its name, and resolve the current interface name from that address at every
startup (and, for the mid-run vanish case, on every periodic re-check) rather
than trusting a name that was correct once.

- **Direction needed**: `reac_mac.c` already has the pure primitive for the
  direction "given an interface name, what is its hardware address"
  (`reac_mac_default_src`, `SIOCGIFHWADDR`). The reverse lookup — given a MAC,
  which interface currently carries it — does not exist yet. It is a small
  addition: enumerate interfaces (`if_nameindex()`), read each one's
  `SIOCGIFHWADDR`, and return the first name whose hardware address matches.
  No new capability is needed for the lookup itself (`SIOCGIFHWADDR` needs
  none, same as today).
- **CLI shape**: accept a MAC address anywhere an interface name is accepted
  today for the REAC-carrying NICs — e.g. `--live aa:bb:cc:dd:ee:ff` alongside
  the existing `--live IFNAME` (distinguish by whether the argument parses as
  a MAC, the same test `parse_mac()` already does for `--src-mac`), or a
  dedicated `--live-mac` flag if keeping the two forms visually distinct in
  `--help` matters more than one flag doing double duty. `--tx` would take the
  same treatment for symmetry, since a TX NIC rename is the identical failure
  mode.
- **Resolution timing**: resolve MAC -> current name once at `reac_rx_open()`
  (replacing/feeding the name `reac_capture_open()` binds to), and again on
  the periodic vanished-interface check in `rx_loop` — if the name changed
  since the last check, that is itself the "it moved" case succeeding instead
  of failing, and is worth its own loud, distinct log line.

  **But note what this does not cover** (2026-08-29): when the NIC returns under
  the SAME name, "the name changed" is false and this branch never runs, while
  the socket is just as dead. The periodic check must compare the **bound
  ifindex** — which is what `reac_rx_binding_lost()` now does, independently of
  whether the name was pinned by MAC or given literally. Re-resolving a MAC is
  how you *recover* the right name; comparing indices is how you *notice* you
  need to.

## What DID land, and what it leaves to this proposal

`fix/the-binding-is-an-ifindex` (2026-08-29) makes the mid-run check honest and
terminal:

- the feeder reads the ifindex **from the capture socket itself**
  (`getsockname` on `AF_PACKET`) at open, rather than re-deriving it from the
  name — the binding is the thing under test, so it is the thing observed;
- every ~2 s it compares that against what the name resolves to *now*, so a
  removal, a rename and a **same-name re-enumeration** are all caught, and the
  message distinguishes them;
- on loss it **exits non-zero** instead of logging forever. Detection without
  exit was the expensive half of the 2026-08-29 outage: the daemon knew, said
  so for three minutes, fell silent when the name returned, and then sat there
  for four more minutes with a dead socket while the operator replugged a
  healthy box. `packaging/reac-pw.service` already carries
  `Restart=on-failure` + `RestartSec=2`, so the non-zero exit is all that was
  missing for the packaged path to heal itself.

  **The transient units this rig runs by hand do not have that.** The two
  segments are started with `systemd-run --user`, which defaults to
  `Restart=no` — so on 2026-08-29 nothing would have restarted the daemon even
  if it had exited. Pass `-p Restart=on-failure -p RestartSec=2` when starting a
  segment that way, or run the packaged unit.

This proposal is still the right next step for the *other* half — surviving a
name that changes across a reboot, so the restart has a correct name to bind.

## Why this is a proposal, not a fix landed here

This branch (`fix/loud-on-vanished-interface`) scopes to making a name-based
failure **honest** — refuse loudly at startup, alarm loudly mid-run — because
that is what an unattended run needed most and it required no new resolution
plumbing. Pinning by MAC is a genuine improvement on top of that (it can
survive the rename that the loud-failure fix can only report), but it touches
the CLI surface (`--live`/`--tx` parsing, `--help`, `reac_mac.c`'s public
surface) and deserves review of the flag shape on its own, not folded
silently into a bug-fix branch.

`~/.config/openmixer/rig.env` (the operator's own file, not this repo) is
where `REAC_LIVE_IFACE` lives today; whichever resolution this proposal lands
as, that variable's *contract* — "names the REAC NIC" — does not need to
change, only what a valid value looks like (a name, or a MAC, or either).
