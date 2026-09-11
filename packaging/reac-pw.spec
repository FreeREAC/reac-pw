# SPDX-License-Identifier: GPL-3.0-or-later
# reac-pw — PipeWire-native REAC endpoint, for the Fedora MiniPC target.
Name:           reac-pw
# Overridable at build time -- the tarball/CI wrapper passes
#   --define "version_override $(git describe --tags ...)"
# so releases version from git tags; the fallback tracks meson.build's version.
Version:        %{?version_override}%{!?version_override:0.5.12}
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
BuildRequires:  pkgconfig(libreac) >= 0.9.0
# libreac-transport (docs/design/specs/2026-09-11-reac-transport-library.md, 0.5.11): the
# sockets, SCHED_FIFO pacer, RT threads, VLAN/topology scan, ring and segment lock that used
# to be built here as src/*.c now come from this package; 0.5.10 and earlier never linked it.
BuildRequires:  pkgconfig(libreac-transport) >= 0.9.1
Requires:       pipewire
# THE SONAME IS NOT THE FLOOR. rpm generates libreac.so.1()(64bit) from the link and that
# is all it generates: 0.7.2 carries soname 1 too, satisfies it, and the daemon then dies
# at exec on an undefined reac_link_* -- the exact 0.6.0 failure the %%description below
# recounts, one soname later. The version floor has to be written down.
Requires:       libreac >= 0.9.0
Requires:       libreac-transport >= 0.9.1

%description
reac-pw exposes a Roland REAC stream as PipeWire graph nodes: reac:capture
decodes the master's 40-channel downstream broadcast into mono DSP sources, and
reac:playback encodes graph audio back onto the wire (EtherType 0x8819). It also
carries the virtual-stagebox JOIN/HOLD connection FSM so the node can present
local inputs to a real Roland master. Built for a Fedora MiniPC running a
PREEMPT_RT kernel + PipeWire.

Links dynamically against the system libreac (>= 0.8.0), which carries the
shared REAC byte-layout core AND, since 0.8.0, the whole CONTROL PLANE — the
enrolment FSMs, the hunt, the grant and the frame builders behind
<reac/reac_link.h>. This daemon is sockets, the pacer and PipeWire; it does not
decide the protocol. The byte-layout half is: frame validation, 24-bit decode of the braid in
both directions (downstream and box upstream), the braided encode, the
f32<->s24 sample pair, the OHRCA +2 length rule, capture and pcap replay.
Nothing is vendored.

The floor is 0.8.0 because that is the release the control plane moved into, and
a daemon built against it will not resolve reac_link's symbols in anything older.
The 0.7.0 floor below is kept as the history of why a floor exists at all:
it was the release that removed
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
* Fri Sep 11 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.11-1
- THE TRANSPORT LAYER MOVES OUT TOO. Operator ruling: a second library,
  libreac-transport, in the libreac repo. reac_ifscan, reac_topo, reac_vlan, reac_slave,
  reac_pacer, reac_tx, reac_rx, reac_linkmon, reac_segment_ident, reac_seglock,
  reac_role_swap, reac_ring, reac_rt, reac_pace_watch, reac_ifname, reac_conf, reac_mac and
  the local reac_link (renamed reac_carrier) are gone from src/ and come from
  libreac-transport >= 0.9.0 unchanged; this package keeps only the PipeWire binding
  (reac_sink_node, reac_source_node, reac_headamp_prop, reac_rate_cfg, reac_role_cfg's
  PipeWire-facing part, main.c's node half) and CAP_NET_RAW/CAP_NET_ADMIN, which the
  binding process still holds and the linked library runs inside. No behaviour change:
  same test names, same counts (69 tests, 68 ok, 1 skipped), the --help/env vocabulary
  byte-identical. See docs/design/specs/2026-09-11-reac-transport-library.md (in libreac).

* Wed Sep 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.7-1
- THE DAEMON NO LONGER SPEAKS REAC CONTROL. Operator ruling: sockets and PipeWire only. The
  JOIN/HOLD table, the master establishment and grant sweep, the hunt and arbitration, the box
  registry, the clock discipline and the virtual-stagebox builders are gone from here and live
  in libreac >= 0.8.0 behind <reac/reac_link.h>. What stays is what a daemon is: AF_PACKET
  sockets, the SCHED_FIFO pacer, netlink, PipeWire nodes and the lifecycle around them.
- No behaviour change is intended by the move: the files went across unchanged, because they
  had been written pure from the start.

* Wed Sep 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.6-10
- A joining box's fillers carry three states in their control area and we sent one: zero
  before the announce, 0x52 while requesting, 0x7a once granted. We sent zero throughout,
  which is the only field-level difference across 40001 frames of ours and of the enrolment
  that IS granted - and replaying that granted file with the 0x52 window zeroed is refused,
  the only variant of it that is. The S-0808 as master tolerates zeros, which is why the
  first rig round enrolled and the S-1608 refuses.

* Wed Sep 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.6-9
- A box-master wire is LISTENED to before it is spoken on. Replays settled that neither the
  declaration nor the phase is refused - the S-0808's own flood-less enrolment was granted at
  three different phases, and so was the same file carrying the master's own port table - so
  what is left is what no granted sequence does: flooding at a master that is calling, and
  announcing on our own clock inside its scene transfer.
- The wire stays empty for two announce cadences; a cfea inside that window ends the flood
  before it starts, and the flood is kept for a SILENT master because that is how one is
  found. The announce waits 200 ms without a scene record. The burst is asked once and
  retried only after 2 s with no echo, and never re-floods.
- The FSM's flood bound is counted from when we speak: it advances per tick whether or not a
  frame left, so the listening window was spending the flood it exists to decide about.

* Wed Sep 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.6-8
- A box-master segment's doors are sized from the model's OUTPUT count, not from the width
  the box broadcasts. An S-1608 master's playback door came up at sixteen channels where that
  box has eight outputs; an S-0808 hid it. Both real captures carry 8 slots to an 8-out
  master. The capture door also names the box now, as the master path's does.
- The config-announce declares OUR inventory, not a borrowed one (libreac >= 0.7.2 picks the
  table from the width): we sent the S-1608's table to everyone, and on that box's own wire
  it was its own identity announced back at it.
- A role change on a HEARD segment drops it so the wire is CLASSIFIED again. A pin changed
  from master to auto at runtime re-opened into the DESK-slave engine, because
  join_box_master is the hunt's verdict and the in-place swap never re-ran it; a restart took
  the right path. The stale sniffer is closed with it, or the re-hear answers with the old pin.
- reac_slave's STATE transcript carries its segment tag; with two box-master segments the
  lines were unattributable.

* Wed Sep 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.6-7
- Both doors of a box-master segment publish the same link state. After the engine enrolled,
  reac-playback was still at its create-time `probing` while the capture door said
  `established` - on that path the sink runs with no pacer and so no badge timer to move it -
  and a console folds the two nodes into one row, so the pair read as a resync in progress.
  Pushed through the same composer the capture node uses; a second spelling of those keys is
  what produced the mismatch.

* Wed Sep 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.6-6
- The cold-connect burst sends THREE DISTINCT records - tags 0100, 0000, 0302, as spec/reac.ksy
  states and as a real S-1608 sends them. It sent the first record twice, and a real S-0808
  echoes one per distinct record, so it was asking for a two-record answer where a real box
  asks for three. The missing builder was libreac's gap and is fixed there; requires libreac
  >= 0.7.2.
- No golden blocks remain in this package: the burst's middle record is generated by libreac
  from the DT1 container, its tag and the Roland checksum, and the box-master config-announce
  is a captured block carried in libreac beside the matrix it differs from.

* Wed Sep 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.6-5
- WE WERE CLAIMING THE LINK BEFORE IT WAS GRANTED. Our unicast fillers carried the ESTABLISHED
  descriptor (00 7a x 16) from the first frame; the box that WAS granted sent zeros there for
  the whole cold-connect and started the descriptor only after its grant. The claim now
  follows the FSM in either carrier, because it is a statement about the pairing and not about
  the geometry it rides in.
- The veth emulator was granting on the control bytes alone and would have passed all three
  refused builds. It now refuses a peer whose pre-grant frames carry the descriptor, and the
  proof prints where the descriptor appears against where the grant was given.
- REACPW_BOX_MASTER_BURST=free|chanmap, unproven and labelled so: the granted box's burst
  landed 1.0 ms after a chanmap, which is one sample, so it is a knob rather than a change.

* Wed Sep 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.6-4
- REAC_SRC_MAC / REAC_SRC_MAC_<segment>: the source address per wire, from the layered conf,
  because --src-mac has always existed and the packaged daemon takes no arguments. It
  overrides both defaults, and its first use is trying a granted box's own address on a
  box-master wire.
- No behaviour change otherwise. DESIGN.md records the byte diff of the box-shaped run
  against the granted S-1608: one non-cell difference, our unicast FILLER carrying the 007a
  ESTABLISHED descriptor before the grant where that box carried zeros until after it, and no
  probe-response relation - the S-0808 sends nothing to answer in the two seconds before the
  announce, so none was implemented.

* Wed Sep 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.6-3
- REACPW_BOX_MASTER_FRAME=mixer|box, the run that separates two readings the rig has not
  been able to tell apart. Two builds were refused by the real S-0808 and each differed from
  the box that WAS granted in a different field: 0.5.6-1 sent 340 B frames with a declaration
  derived from the master's width, 0.5.6-2 sent 1492 B frames with the S-1608's declaration
  byte-identical. Nobody has run 340 B frames with the right declaration. `box` is that
  fourth corner - an exact S-1608 imitation, unicast after the flood - and `mixer` (the
  default) is the operator's ruling unchanged. Everything else is identical between them, so
  the two runs differ in exactly the field under test, and no rebuild is needed between.
- The 0.5.6-2 capture is decoded in DESIGN.md: our announce and both cold-connect records are
  byte-identical to the S-1608's, from a Roland-OUI source, unicast to the box, retried every
  0.8 s with the announce 200 ms ahead of each burst. What the last two rounds set out to put
  on the wire is on the wire.

* Wed Sep 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.6-2
- WE ARE THE MIXER ON THAT WIRE. Operator: "mixer always sends 40ch, boxes send their width
  only." Every audio frame on a box-master wire is the fixed 1492 B 40-slot downstream, the
  box's outputs in their slots, in the presence flood and after the grant alike; the 340 B
  the S-1608 sent that box is what a BOX sends. Only the ENROLMENT is the box's: flood,
  unicast config-announce, cold-connect burst ~200 ms later, the grant echoed in the box's
  broadcast, then the 1 s heartbeat.
- The declaration is the one that was granted, byte for byte: the 34-byte block a real S-1608
  unicast to the real S-0808 four milliseconds before it was echoed, carried as a golden.
  0.5.6-1 derived it from the MASTER's width and announced selector 0x84 - the family of the
  box it was talking TO - and the rig sent four correct bursts behind it, lamp blinking, and
  was never granted. libreac's builder emits 0x82 at 16 channels, which is not it either.
- The slave's source MAC on this one path is Roland's OUI over this NIC's host part, the one
  documented exception to reac_mac.h's verbatim-address law, with the socket promiscuous
  because a box unicasts to the address it was announced from.
- Fixed on the way: a control frame stamped onto a downstream kept BROADCAST in its own dst
  bytes and arrived labelled broadcast however it was sent; and the heartbeat rides two FSM
  decisions, so handling one left a segment established and silent.

* Wed Sep 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.6-1
- A WIRE A STAGEBOX MASTERS IS NOW ENROLLED WITH, THE WAY A STAGEBOX ENROLS. Operator
  ruling: "It is only a matter of following the same protocol that we expect." A 75 s
  capture of a real S-1608 in slave mode meeting the real S-0808 in master mode, with
  reac-pw stopped, is the recipe and it is followed exactly: read the master's width off
  its broadcast (340 B = 8 slots), flood broadcast FILLER at THAT width, stop broadcasting
  and unicast the config-announce, ~200 ms later the cold-connect burst, accept the grant
  the master echoes back inside its own broadcast, then unicast at the wire rate with a
  1 s heartbeat. The segment's reac-playback node feeds that upstream, so an operator can
  route to the box master's outputs.
- 0.5.5's desk downstream to a box master is RETIRED. It was the wrong shape: the capture
  shows a box on M sends no announce at all, never changes a byte of its broadcast on
  enrolment, and grants a SLAVE. The pacer is the master role's again.
- NO HEAD-AMP KEYS ON A BOX-MASTER SEGMENT. 0.5.5 published channels=8/base=0 from the
  model row; the write then reached the node and the real S-0808's preamp did not move
  (floor -91.4 dBFS at gain 32, 52 and 32 again, against +18.9 dB on an enrolled S-1608 by
  the same path), and the capture says why: no head-amp record travels in either direction
  on that wire. A capability the wire cannot carry is a control that moves nothing.
- reac.link-state FOLLOWS THE ENGINE, NOT THE HEARING. The rig: "S-0808 is not enrolled but
  omx sees it available", with the box's own lamp unlocked. `probing` while listening,
  flooding and waiting for the grant; `established` only once the unicast stream and the
  heartbeat run. 0.5.2's rule that a receive-only join IS the slave role performed is
  overturned with it, and the predicate that encoded it is gone.
- Measured on a veth against the box-master emulator, which now grants the way the S-0808
  does: flood 5142 frames of 340 B broadcast; announce then a 3-record cold-connect burst;
  established with a heartbeat; a 0.5 FS tone into reac-playback read back off the UPSTREAM
  at -17.0 dBFS on slots 0/1 with -999 on an unfed slot and 19.99 dB of delta for 20 dB at
  the source; 0 frames before the box spoke; link-state probing during the recipe and
  established after; two drop/rejoin cycles carrying audio both ways.

* Wed Sep 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.5-1
- A STAGEBOX THAT MASTERS THE WIRE NOW GETS THE SAME DOWNSTREAM WE SEND A BOX WE MASTER.
  Operator ruling: sending is always the same, and being clock slave is only part of the
  enrolment. A box with its REAC Mode switch on M provides the cadence instead of following
  one, and it consumes the desk-to-box downstream whoever owns the clock -- so the segment
  keeps following the box's clock (its arriving frame IS the slot; no pacer deadline) and
  now also builds and broadcasts the ordinary 1492 B master downstream on it, one frame per
  frame received. Nothing at all leaves before the box's first frame: a timeout is not a
  slot.
- The segment therefore publishes reac-playback.<segment>, sized to the box's outputs from
  the width it broadcasts, and the reac.headamp.<ch>.<param> control keys with it. An
  operator can route to that box and set its preamps without touching the switch on its
  front panel. What does NOT change: no handshake is attempted in either direction, no
  slave engine is opened, and the segment's answer stays on its capture node --
  reac.master.state=foreign, reac.pace.source=foreign-master. This wire is driven, not
  owned.
- Measured on a veth against the box-master emulator, which now decodes what comes back
  through libreac's own oracles: emission ratio 1.0000 downstream frames per box frame over
  2 s, 1492 B, zero frames before the box's first, a tone played into the playback node read
  back off the wire on the slots wire-format.md places it on and 20.01 dB down when the
  source drops 20 dB, a head-amp write decoded out of a control block the daemon sent, and
  zero frames sent in 1.5 s of a silent box against 2624 in the same window with it back.
- A box-master segment dropped by a link loss past the hold and heard again is proven to
  carry audio in BOTH directions afterwards, twice in a row. The rig defect of 2026-09-09
  13:36 that prompted it (a rejoined segment publishing digital silence and its create-time
  property seeds over a live wire) does NOT reproduce on a veth under either condition that
  rig can present, and is recorded in DESIGN.md as an open measurement rather than a fix.

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
