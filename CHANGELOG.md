# Changelog

## Unreleased (1.0.33)

- A stagebox with no outputs, like a 40 in / 0 out S-4000S, no longer leaves a stereo output on the
  graph. A build that asked for its playback node at zero channels got a two-channel sink named
  reac-playback and described as "REAC segment door (no box recognized yet)"; a zero width now means no
  playback node at all, for a box found on the wire and for one pinned with --box alike.
- For such a box, reac-capture carries everything reac-playback carries for a box with outputs: the
  segment's master state, rate, role, discovery, health and head-amp properties, and it takes the
  reac.cfg.rate, reac.cfg.role and reac.headamp writes.

## 1.0.32 - 2026-10-08

- reac-pw is now installed from the FreeMixer package channel: signed RPMs for Fedora 44 (x86_64,
  aarch64) and DEBs for Debian bookworm and trixie, including Raspberry Pi OS (amd64, arm64). Add the
  channel once, then install or update with dnf or apt. The daemon itself does not change.
- The Debian package installs the same user service as the RPM, gives the daemon the file capabilities
  it needs to run without root, and starts the service for the user who is logged in when exactly one
  user is. With nobody or several users logged in it changes nothing, and the one command to run is
  `systemctl --user enable --now reac-pw`.
- reac-pw has a manual page, `man reac-pw`.
- Needs libreac and libreac-transport 1.7.0 or later, from the same channel.

## 1.0.31 - 2026-10-07

- 1.0.30 was tagged but never published: its source tarball lacked BUILDING.md, so the package's own
  checks failed while it was being built. 1.0.31 is the same code with the tarball fixed, and it carries
  everything listed under 1.0.30.

## 1.0.30 - 2026-10-07

- Every stagebox is now described by what it says on the wire. Its capture and playback channels follow
  the inputs and outputs it declares, its name comes from its own identity (S-1608, S-0808, S-4000S-3208,
  S-4000S-1624 ...), and its firmware and hardware block are shown beside it. A box that declares no
  outputs, like a 40 in / 0 out S-4000S, gets no playback node.
- The box's name is published as reac.box-name, next to reac.box-model.
- A box whose family has never been seen is named by its channel counts (REAC-0816) and the log asks for
  a capture of it.
- The built-in model list is no longer used for a connected box. If it disagrees with the box, the box
  wins and the log says "catalogue defect".
- An S-0808 now shows its firmware and hardware block: its identity is asked for again after it joins,
  and its name, sent in two pieces, is read.
- A stagebox in master mode is listened to again on libreac 1.6 and later.
- Needs libreac 1.7.0.

## 1.0.28 - 2026-10-06

- The source tarball carries the protocol's numbers (facts/) and NOTICE. 1.0.27's tarball had neither and
  its package build stopped, so 1.0.27 was tagged and never packaged; 1.0.28 is 1.0.27's code.

## 1.0.27 - 2026-10-06

- A box that no model row matches gets nodes sized from its declaration: the capture node takes the
  declared inputs, the playback node the declared outputs, and the box is named S-4000S-<in><out> unless
  a Roland row has the same widths. A box that declares no outputs (40 in / 0 out) gets no playback node.
- A box with no playback node carries its identity on the capture node: firmware, REAC version, hardware
  block and MAC are stamped on both nodes, and a capture node rebuilt after a configuration change is
  stamped at once instead of staying blank.
- Built against libreac 1.5.1, which grants a box up to 40 channels wide.

## 1.0.26 - 2026-09-24

- A playback node that fails alone no longer tears down a healthy capture node. Recovery judges each
  side of the pair, and each rebuild line names its own node and reason.
- The pacer's fallback from the ETF qdisc says what happened: it reports a qdisc as removed only when
  this daemon installed it, and a failed removal is retried at exit.
- The log line about too many VLANs says the extra segments are not served.

## 1.0.25 - 2026-09-23

- A node whose PipeWire server died is no longer taken for one still on the graph. After a PipeWire
  restart the pair is rebuilt, where before a box could stay established with no node for the better
  part of an hour.
- The daemon no longer restarts a wire it is not sending on, and says once per change why it holds.
- A declared VLAN is created only after its tag is heard on the trunk; the declaration pins the role.

## 1.0.24 - 2026-09-22

- A VLAN heard from any tagged frame is a segment: a trunk's cold VLANs appear from the switch's own
  traffic instead of having to be declared, and the journal says "REAC heard on this vid" when the frame
  arrives. Needs libreac and libreac-transport 1.5.0.
- A wire carrying 2000 frames a second is no longer taken for silent while a segment is being served,
  which made the daemon take the master role and yield it back.

## 1.0.19 - 2026-09-17

- Installing reac-pw no longer enables the service for every user of the machine. A second daemon, started
  under root's user manager by `sudo dnf install`, had won the segment lock and locked the console user
  out. The package now only removes any such leftover from an older install; you enable the service once,
  as the console user, with `systemctl --user enable --now reac-pw`.
- The daemon refuses to start as root.
- When the segment is already held, the refusal names the holder's process and user where the system
  allows it.

## 1.0.0 - 2026-09-11

- The PipeWire-native REAC endpoint on libreac and libreac-transport 1.0.0: trunk VLAN segments, master
  and slave at 44.1, 48 and 96 kHz, and head-amp control on the wire.

## Earlier releases

- Versions before 1.0.0 and the notes of each release in between are kept in the git history.
