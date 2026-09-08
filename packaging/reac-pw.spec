# SPDX-License-Identifier: GPL-3.0-or-later
# reac-pw — PipeWire-native REAC endpoint, for the Fedora MiniPC target.
Name:           reac-pw
# Overridable at build time -- the tarball/CI wrapper passes
#   --define "version_override $(git describe --tags ...)"
# so releases version from git tags; the fallback tracks meson.build's version.
Version:        %{?version_override}%{!?version_override:0.5.0}
Release:        2%{?dist}
Summary:        PipeWire-native Roland REAC endpoint (RX source + TX sink + stagebox FSM)

License:        GPL-3.0-or-later
URL:            https://github.com/FreeREAC/reac-pw
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  meson >= 0.60
BuildRequires:  ninja-build
BuildRequires:  gcc
BuildRequires:  pkgconfig(libpipewire-0.3)
BuildRequires:  pkgconfig(libspa-0.2)
BuildRequires:  pkgconfig(libreac) >= 0.7.1
Requires:       pipewire

%description
reac-pw exposes a Roland REAC stream as PipeWire graph nodes: reac:capture
decodes the master's 40-channel downstream broadcast into mono DSP sources, and
reac:playback encodes graph audio back onto the wire (EtherType 0x8819). It also
carries the virtual-stagebox JOIN/HOLD connection FSM so the node can present
local inputs to a real Roland master. Built for a Fedora MiniPC running a
PREEMPT_RT kernel + PipeWire.

Links dynamically against the system libreac (>= 0.7.0), which carries the
shared REAC byte-layout core: frame validation, 24-bit decode of the braid in
both directions (downstream and box upstream), the braided encode, the
f32<->s24 sample pair, the OHRCA +2 length rule, capture and pcap replay.
Nothing is vendored.

The floor is 0.7.0 because that is the release that removed
reac_ctrl_build_name_frame() and reac_ctrl_build_extra_frame() and replaced them
with reac_ctrl_build_identity_first(), which this package calls. libreac shipped
that break once as 0.6.0 with its soname still 0, and every guard was inert at
the same moment: this floor accepted the library with and without the symbols,
the unchanged NEVRA made `rpm -U` a no-op, and the installed binary loaded the
new libreac.so.0 and died on `undefined symbol`. 0.7.0 carries soname 1, so
rpm's generated runtime requires now refuses a mismatched pair at install time
rather than at exec.

Earlier floors, subsumed: 0.6.0 was the release that actually SHIPPED the
control-block core in the shared object (reac_ctrl_*, reac_headamp_*,
reac_ports_parse) -- up to 0.5.0 the spec's hand-kept object list left
reac_ctrlblk.o and reac_ports.o out of libreac.so while -devel installed the
headers declaring them, so a build resolved every include and then failed at
link. 0.5.0 was the floor for the decode side (the release whose downstream
decode reads the same braid its encoder writes; a 0.4.x libreac-devel links
happily and mis-decodes every downstream frame).

Keep this in step with meson.build's dependency() floor.

%prep
%autosetup -n %{name}-%{version}

%build
# Explicit meson (the host uses a pip-installed meson, not the dnf macros).
# %%set_build_flags exports the Fedora CFLAGS/LDFLAGS (incl. -g and the linker
# build-id) so the plain buildtype still yields a real debuginfo package.
# --wrap-mode=nofallback: the libreac dependency MUST resolve to the system
# libreac-devel (pkg-config), never the bundled subproject wrap -- the RPM links
# libreac dynamically (runtime dep auto-generated from the libreac.so.1 soname).
%set_build_flags
meson setup _build --prefix=%{_prefix} --buildtype=plain --wrap-mode=nofallback
meson compile -C _build

%install
DESTDIR=%{buildroot} meson install -C _build
# Deliberately NO unit and NO /etc/reac-pw here: the canonical integration is
# openmixer-server's packaged USER unit (reac-pw-master.service, driven by
# ~/.config/openmixer/reac.env from Setup -> Adapters). Shipping the legacy
# SYSTEM unit too would put two daemons in contention for the REAC NIC.
# packaging/reac-pw.service stays in-repo as the standalone/no-openmixer
# reference; install it by hand if you run reac-pw without openmixer.

%check
meson test -C _build

%files
%license LICENSE
%doc README.md
# File capabilities, applied by rpm itself (%%caps survives rpm -V / --restore;
# no setcap scriptlet needed): raw 0x8819 capture/emit without root (cap_net_raw)
# + SCHED_FIFO for the cadence pacer (cap_sys_nice). openmixer's packaged
# reac-pw-master.service ExecStartPre getcap-guards on exactly these, and
# scripts/deploy-live.sh refuses a live restart without them.
%caps(cap_net_raw,cap_net_admin,cap_sys_nice=ep) %{_bindir}/reac-pw

%changelog
* Tue Sep 08 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.0-2
- The recovery for a capture node that never reached the graph now RE-BUILDS it: the
  ensure it went through only rebuilds on a width or label change, and neither moves
  when a node simply fails to appear, so the node is destroyed first. The retry is
  bounded -- a 2 s grace, a window that doubles to 32 s, five attempts, then one line
  naming PipeWire's reason -- and the "autodetected ... -> reac-capture N in" line is
  said once per box and only once the node is really there.
- A rebuilt capture node is re-stamped with the box's badges. The sink's badge push
  rides a change guard, so a recovered node came back reading box-model "none",
  width "0x0" to every client.
- No stdio on the realtime callback: the graph-clock diagnostic is composed into a
  fixed buffer and printed by the main loop, and the pacer the RT path reads is set
  before the stream is connected.
- An unusable graph driver publishes nothing instead of "not present", so a segment's
  two nodes in different driver groups cannot flap the reference between them.
* Tue Sep 08 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.0-1
- NOTHING IS CONFIGURED. Started with no flags and an empty conf, the daemon finds
  its own segments -- every linked Ethernet interface is sniffed passively, and the
  first REAC frame heard turns that interface into a segment -- and then takes the
  end of the pairing the wire leaves open: a desk mastering it is joined as a slave,
  a wire with a box on it and no master is taken as master after a three-second hunt
  (three master announce cadences) and the box is granted, and a stagebox strapped to
  master mode is refused with the remedy named, never fought. REAC_IFACES and the
  per-interface env files are gone; a per-segment fact is a suffixed key in the one
  conf. REAC_ROLE_<segment> still wins outright; a bare REAC_ROLE is a floor and is
  superseded out loud.
- A segment IS its interface and is NAMED after it: node pair reac-capture.<iface> /
  reac-playback.<iface>, per-segment conf key REAC_ROLE_<iface>, and no <iface>.env file
  read anywhere. Node names that follow the BOX are owed and are listed with the other
  owed increments in DESIGN.md; those go 0.5.1, 0.5.2, ... -- the middle digit does not
  move again for them.
- A segment is BOTH of its nodes. reac-capture publishes reac.segment in the master
  role too, so a client keying a stagebox off the reac.* identity finds one segment
  and not half of one; and a capture node that fails to reach the graph is reported
  with PipeWire's own reason and REBUILT, instead of a journal line claiming a width
  over a node nobody can patch.
- The clock reference no longer waits for the playback side to be patched. An
  unlinked reac-playback is suspended and its RT callback never runs, so the
  graph-clock sample is taken by whichever of the segment's nodes the graph drives.
- reac-pw with no arguments STARTS. It used to answer the usage text and exit 2,
  which is what the packaged unit passes, so the service could not come up at all.
- The clock discipline is ON by default. A daemon that owns a segment's pace and
  free-runs it is misconfigured in principle: every box on the wire locks to that
  rhythm. The best-reference ladder (NIC/external PHC > a hardware-driven graph clock
  > the box's counter slope) is what the live rig has run since 2026-09-07 20:55
  without incident. Every safety it shipped with stands -- a structurally unusable
  reference is refused whatever is designated, measured instability outranks the
  designation, the period is slewed and never phase-stepped -- and free-run is now
  announced rather than silent. REACPW_CLOCK_FOLLOW=0 opts out.
- Boolean knobs are read one way. REACPW_CLOCK_FOLLOW=0 used to mean ON, because
  that reader only asked whether the variable was set.
- Wireless interfaces are excluded from the autodetect scan unless opted in
  (REAC_IFACES_ALLOW_WIRELESS): ARPHRD_ETHER is true of Wi-Fi too, and Wi-Fi's
  jitter has no repacer here.
- Discovery trusts no MAC. REAC gear is recognized by protocol frame alone, with a
  per-segment peer lock closing the checksum-exempt FILLER gap that leaves.
* Mon Aug 31 2026 Pau Aliagas <linuxnow@gmail.com> - 0.4.8-1
- The binding is an IFINDEX. An AF_PACKET socket binds to an index resolved once
  from the name, so a USB NIC re-enumerating under the SAME name and MAC left the
  daemon deaf AND mute while every name-based check stayed happy. The feeder now
  records the index it is actually bound to and exits non-zero when it moves, so
  the unit restarts instead of narrating a dead segment.
- Grant on the box's DECLARATION rather than a wall-clock dwell
  (REACPW_GRANT_ON_DECLARE=1, default off). Establishment measured 1.684 s ->
  0.134 s on an S-1608 and 1.756 s -> 0.206 s on an S-4000S, audio-verified
  through a physical loopback in both conditions. The dwell stays the CAP for a
  box that has not declared.
- A rival master is classified by its frame GEOMETRY and published as
  reac.master.rival.kind / reac.master.refusal. A stagebox strapped to master
  mode claims master by every control-frame rule while emitting a box width, and
  a master never joins another master, so it is reported as a misconfiguration
  rather than followed.
- Requires libreac >= 0.7.1 for the geometry helpers.
* Sun Aug 23 2026 Pau Aliagas <linuxnow@gmail.com> - 0.3.0-1
- Builds against the SYSTEM libreac >= 0.7.0, and the version moves so that
  installing it is not a no-op. libreac 0.7.0 removed
  reac_ctrl_build_name_frame() and reac_ctrl_build_extra_frame(); this package
  calls reac_ctrl_build_identity_first() instead, so the two have to be
  installed TOGETHER -- a 0.3.0 binary cannot run on libreac 0.6.0 and the old
  0.2.0 binary cannot run on 0.7.0.
- libreac 0.7.0 carries soname 1, so the runtime requires rpm generates from it
  (libreac.so.1) now refuses a mismatched pair at INSTALL time. Under 0.6.0's
  soname 0 the pair installed cleanly and the binary died on `undefined symbol`
  at exec, which is the failure this release exists to make impossible.
- A control frame is told apart by its opcode, not by how long it is.

* Sat Aug 22 2026 Pau Aliagas <linuxnow@gmail.com> - 0.2.0-1
- Builds against the SYSTEM libreac >= 0.6.0, the first release whose shared
  object actually contains the control-block core. The floor is checked twice
  over on purpose: pkg-config refuses an honestly-old library at configure time,
  and the 25 reac_ctrl_*/reac_headamp_*/reac_ports_parse imports refuse a
  library that merely CLAIMS the version at link time. The second check is the
  load-bearing one -- the version string in the subproject shim is hardcoded, so
  a stale checkout reports the right number and links nothing.
- subprojects/ is still absent from the tarball, so the RPM has no fallback to
  take and cannot quietly vendor.

* Sun Jun 14 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.0-1
- Initial package: PipeWire-native REAC endpoint for the Fedora MiniPC.
