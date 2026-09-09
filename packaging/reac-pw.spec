# SPDX-License-Identifier: GPL-3.0-or-later
# reac-pw — PipeWire-native REAC endpoint, for the Fedora MiniPC target.
Name:           reac-pw
# Overridable at build time -- the tarball/CI wrapper passes
#   --define "version_override $(git describe --tags ...)"
# so releases version from git tags; the fallback tracks meson.build's version.
Version:        %{?version_override}%{!?version_override:0.5.4}
Release:        1%{?dist}
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
* Wed Sep 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.4-1
- THE WIRE IS OURS ONLY WHILE NOBODY ELSE CLAIMS IT. Every segment served as MASTER on an
  interface nobody pinned keeps its passive sniffer now, not just one taken on proven
  silence, and a desk (or a stagebox on M) that starts mastering it is yielded to: master
  down, slave up, no shouting. The venue case is the whole argument -- a house console
  hears the stageboxes, grants them and drives, and the Roland desk is switched on
  afterwards. A pinned segment keeps the operator's answer; the only thing a rival does to
  one is the 0.5.1 refusal.
- AND A YIELD IS NOT A ONE-WAY DOOR. The sniffer is kept ACROSS the yield, so when the
  desk goes home and its sighting ages out of the discovery table (5 s, the same bar "a
  device is really gone" means everywhere else here) the wire is taken back as master,
  through the same drop-then-serve seam a cold start uses. It does not re-enrol the boxes
  by itself -- a stagebox announces on its own PHY-up edge and on nothing else -- but it
  is DRIVING for them when they do, and that is measured on the peer's own capture.
- The decision is a pure table (src/reac_watch.{h,c}): yield, retake, unrefuse or stand.
  Inline in main.c the only thing that could exercise it was a 70 s veth run, and that run
  cannot choose which route took the wire.
- reac.pace.source STOPS BEING A CONSTANT. The playback door built its arbitration with
  REAC_PACE_FREE_RUN written in -- true under the 2026-08-21 config of record in which
  clock-follow was off, and false since 0.5.0 made following the default: on the rig
  2026-09-08/09 the journal read "locked to graph clock (api.alsa.0)" while the console's
  segment row read "free-run". The pacer mirrors its discipline into an atomic and the
  door maps it: phc, graph-ref or box-slope where the DLL is LOCKED, free-run while
  acquiring, in holdover or with following off, and foreign-master wherever somebody else
  times the wire.
- Three defects the new proof found on the way. A served interface goes LISTEN -> SERVE ->
  DROP and never through UNLISTEN, so every segment that ever dropped leaked its topology
  tap AND its topology-table row -- both bounded at eight, and past the eighth every trunk
  is served as one flat segment; the table's refusal was silent as well. And a
  sub-interface could be tapped where /sys was unreadable, which minted a VLAN on a VLAN;
  the daemon now refuses that from a fact it already holds.
- meson test: 67 tests, 66 ok, 1 skipped (reac_pacer's live cadence, needs CAP_NET_RAW).
* Wed Sep 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.3-1
- THE TRUNK: a VLAN is a segment, and the daemon makes the netdev it needs. Nothing in
  src/ read a VLAN tag before this release. Every physical parent with carrier now gets a
  read-only ETH_P_ALL tap, BPF-filtered to 0x8819 and reading PACKET_AUXDATA, which is the
  only socket that can tell a tagged frame from an untagged one: measured again on
  7.2.4-200.fc44, a socket bound to 0x8819 on the parent receives every tagged frame with
  the 802.1Q header absent from the buffer and vlan_tci none, so a detector built on the
  obvious socket reports every trunk as an access port.
- Each VID heard becomes <parent>.<vid>: ADOPTED where the host pre-created it, CREATED
  over rtnetlink and marked reac-pw:minted where it did not exist, brought up either way,
  and then served as an ordinary interface by the hearing that was already there. A netdev
  we minted is removed on a clean exit and after 30 s of silence on its VID; one we adopted
  is left exactly as it was found. A netdev carrying our alias at startup is a leaked mint
  from an unclean exit: re-owned rather than inherited for ever.
- A parent carrying tagged REAC is never itself a segment. It receives every
  sub-interface's frames with the tag gone, so serving it would put one master over several
  VLANs' boxes; untagged REAC on such a parent is refused by name, and the plain untagged
  NIC with no tagged traffic is unchanged in every respect.
- Without CAP_NET_ADMIN the daemon reports and goes on hearing: it names each VID it cannot
  serve and prints the ip link command that would fix it. Adoption needs no capability, so
  a host that pre-created its sub-interfaces is fully served by an unprivileged daemon. The
  RPM's %%caps line already grants cap_net_raw,cap_net_admin,cap_sys_nice.
- Proven on veth, not on a rig: the whole-binary proof now hears two VIDs on one wire,
  creates a sub-interface for each, serves both as segments at their own widths with audio
  decoding on both, adopts a pre-created one, and on exit removes only what it made.
* Wed Sep 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.2-1
- A JOINED BOX MASTER IS THE SAME BOX IT IS WHEN WE MASTER IT. 0.5.1 joined an S-0808 on
  M and put its eight channels on the graph; the rig showed a segment with no stagebox on
  it, because a console keys a box off reac.box.mac, reac.box-model, reac.box-width and
  reac.link-state and the join published none of the four. Its inputs could not be
  patched. All four are published now, on the segment's one door.
- The identity comes from the WIDTH, because there is nothing else to read: a box on M
  sends no config-announce at all, so what identifies a served box does not exist on that
  wire. An 8-channel broadcast is the S-0808 row of the fixed matrix, exactly -- and a
  width no row matches names NO model rather than defaulting to one, which is what
  libreac's reac_box_model_by_channels would have done.
- reac.link-state reads established once the segment's own feeder is decoding the box's
  frames, probing before that, and never granting: nothing is granted in either direction
  on a wire whose master runs no handshake.
- reac.cfg.role.state no longer sits at role_reestablish_pending over a segment that is up
  and streaming. A receive-only join runs no slave engine and is not waiting for one --
  that engine exists to answer a grant -- so following the box's clock and delivering its
  channels IS the slave role performed, and it publishes applied.
- The audio itself is measured: the box master's own broadcast frames, carrying a distinct
  constant per channel, decoded through the real feeder into the ring the capture node's
  ports are filled from -- eight rows each with its own value, the 32 fabric slots past
  the box's width silent.
- The feeder's REAC_DEBUG counters are keyed by segment. A host running several segments
  printed them unattributed, and a peer that broadcasts never locks the feeder's peer
  address, so the line's src field could not stand in for a name.

* Wed Sep 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.1-1
- A BOX THAT MASTERS THE WIRE IS JOINED, not refused. Operator ruling after the rig
  proof of 0.5.0-3: an S-0808 rebooted with its REAC Mode switch on M, on an unpinned
  wire, was refused -- "masters this wire at 8 ch, which is a BOX width, not a desk's
  40" -- and the daemon then served nothing, so the segment disappeared from the
  console altogether. A clock is a clock whichever end of the pairing sends it. An
  unpinned wire, or one pinned slave, now follows a box master's clock, decodes the
  box-width broadcast it puts on the wire, and publishes a capture node sized from the
  width that box announced (8 ch for an S-0808, not a 40-slot fabric).
- The join is RECEIVE-ONLY, and that is the protocol's own shape rather than a reduced
  one: a stagebox on M runs no handshake at all -- no announce, no grant, no heartbeat
  -- so there is no enrolment to answer and a cold-connect flood aimed at it would be
  noise. Nothing is emitted on a wire we joined this way.
- A wire the operator PINNED master is the one case that still refuses: two answers
  contradict each other and the console never fights a box. The pin drives on link (a
  cold box cannot speak first, which is 0.5.0-3's own rule), the segment's engine
  classifies the box mastering the wire about a second later, and the segment is then
  taken down.
- AND A REFUSED WIRE PUBLISHES A DOOR. The real cost on the rig was the silence: a
  refusal nobody can see is indistinguishable from a daemon that is not running. A
  refused segment now publishes one reac-capture node carrying reac.segment,
  reac.master.state=foreign, the rival's MAC, reac.master.rival.kind=box and
  reac.master.refusal=rival-master-box, with no TX, no pacer, no segment lock and no
  RX feeder behind it. Its sniffer is kept, so the door comes down and the segment
  comes up when the box stops mastering the wire -- no restart, no latch.
- A sighting's WIDTH now crosses the pacer's event ring. It never did: role and model
  index crossed and the geometry did not, so the master side classified every stagebox
  on M as a rival nobody can read (rival-master-unknown) -- the one distinction the
  arbitration exists to make.
- A peer's width also widens on its own in the discovery table, instead of only when
  its model changes. A box on M declares no model at all, so its width could never be
  re-read once recorded.
- Proven end to end on the veth job proof, against a stagebox emitting real
  box-geometry master frames: joined at 8 ch with the right props and not one frame
  sent back; refused on a pinned wire with the door on the graph, our transmission
  stopped, and the segment taken when the box stopped mastering.

* Wed Sep 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.0-3
- A COLD STAGEBOX NOW WAKES. Measured on the rig 2026-09-08 22:10 with 0.5.0-2: an
  S-0808 and an S-1608, both freshly powered, both cabled, both NICs carrier up at
  100 Mb full, and rx_packets moved by ZERO in five seconds on both with no 0x8819
  frame in eight seconds of capture. A REAC box in slave mode spends a bounded
  broadcast flood on PHY-up and then says nothing at all until a master announces to
  it, so a box powered before the daemon can never open the "first classifying frame"
  gate, and both boxes sat mute where 0.4.8 had driven them from the first instant.
- A segment with a REAC_ROLE_<iface> pin is served ON LINK, with no frame waited for.
  Proven on the rig: both boxes came up within two seconds of "pinned master --
  driving on link".
- An UNPINNED linked wired interface that carried NOTHING for 500 ms is taken as
  MASTER, through the ordinary master role -- same pacer, same probing until the box
  cold-connects, same NIC address. A master transmits one frame per audio slot and
  cannot be present and silent, so 500 ms of nothing (1837 consecutive slots at the
  slowest rate served) is proof the port is masterless, not a guess; any frame inside
  the window cancels the licence and the ordinary hunt rules instead. This is what a
  system with no pins on its first boot needs.
  A first cut of this sent ONE master announce every two seconds instead. It was
  measured on the rig and the box never answered -- tx +2 per ~6 s, rx +0 for over a
  minute. A cold box answers a master that is DRIVING. The lone-announce path is gone,
  and with it the Roland-OUI stand-in source address it emitted from, which broke
  reac_mac.h's own law: two hosts on one segment each dismissed the other's frames as
  its own echo, and a box that learned the stand-in dropped on FSM_DROP_MAC_CHANGE
  when it met the served master's real address.
- A wire taken on silence KEEPS its sniffer, because it was served on a bet. If a desk
  turns up on it, the segment is handed over at once -- master down, slave up, never
  fought. A stagebox strapped to master is reported and not yielded to.
- Wireless is excluded twice: out of the scan by default, and never driven on silence
  even where REAC_IFACES_ALLOW_WIRELESS admits one for LISTENING.
- The journal says which of the three an interface did, once, at link: "pinned master
  -- driving on link", "pinned slave -- cold-connect flood, then listening for a
  master", or "unpinned -- listening for REAC"; plus the line naming the silence when
  a wire is taken on it.

* Tue Sep 08 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.0-2
- The recovery for a capture node that never reached the graph now RE-BUILDS it: the
  ensure it went through only rebuilds on a width or label change, and neither moves
  when a node simply fails to appear, so the node is destroyed first. The retry is
  bounded -- a 2 s grace, a window that doubles to 32 s, five attempts, then one line
  naming PipeWire's reason -- and it says so when the node comes BACK, because a rebuild
  followed by silence reads exactly like a rebuild that failed. The "autodetected ... ->
  reac-capture N in" line is said once per box and only once the node is really there.
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
