# SPDX-License-Identifier: GPL-3.0-or-later
# reac-pw — PipeWire-native REAC endpoint, for the Fedora MiniPC target.
Name:           reac-pw
# meson.build's project version is the same number; a release tag is v<Version>.
Version:        1.0.33
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
# >=1.4.0 SINCE 1.0.23: <reac/reac_knock.h> and <reac/reac_tapwait.h> are the library's now
# (operator ruling 2026-09-22 — deciding what a wire is belongs to libreac, this daemon deals
# with enrolled nodes). Against an older libreac the build dies at the #include; the floor is
# raised anyway so the refusal arrives at configure time with a sentence somebody can read.
# >=1.5.1 SINCE 1.0.27: the grant takes a box up to 40 channels wide (a 40 in / 0 out box).
BuildRequires:  pkgconfig(libreac) >= 1.7.0
# libreac-transport (2026-09-11-reac-transport-library, 0.5.11): the
# sockets, SCHED_FIFO pacer, RT threads, VLAN/topology scan, ring and segment lock that used
# to be built here as src/*.c now come from this package; 0.5.10 and earlier never linked it.
BuildRequires:  pkgconfig(libreac-transport) >= 1.7.0
# systemd_user_post/_preun/_postun below, and %%{_userunitdir}/%%{_userpresetdir} in
# %%files -- the RPM now packages its own USER unit (1.0.8, this changelog entry).
BuildRequires:  systemd-rpm-macros
Requires:       pipewire
# THE SONAME IS NOT THE FLOOR. rpm generates libreac.so.1()(64bit) from the link and that
# is all it generates: 0.7.2 carries soname 1 too, satisfies it, and the daemon then dies
# at exec on an undefined reac_link_* -- the exact 0.6.0 failure the %%description below
# recounts, one soname later. The version floor has to be written down.
Requires:       libreac >= 1.7.0
Requires:       libreac-transport >= 1.7.0
%{?systemd_requires}

%description
reac-pw puts Roland REAC stageboxes on your Linux audio graph. Plug a box into a
network port and its inputs appear as PipeWire capture channels and its outputs as
playback channels, sized and named from what the box itself says on the wire, with
its firmware alongside — no configuration, no model list to keep up to date.

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
# THE RPM OWNS THE UNIT (1.0.8). Earlier releases deliberately shipped no unit here,
# reasoning that "the canonical integration is openmixer-server's packaged USER unit
# (reac-pw-master.service, driven by ~/.config/openmixer/reac.env)" -- that unit is
# RETIRED (openmixer's 2026-08-20-reac-master-arbitration,
# amendment 2026-09-02 (second), rule f: "~/.config/openmixer/reac.env is RETIRED,
# with reac-pw-master.service. Its only reader goes; a file with no reader is not a
# store."). The unit openmixer's own adapter drives today is THIS package's
# reac-pw.service (REAC_UNIT = 'reac-pw' in packages/server/src/reac-adapter.ts),
# reading the layered ~/.config/reac-pw/ conf -- and openmixer's own
# docs/install/services.md already documents it as "owned by the reac-pw package,
# not by openmixer". A package that drives a unit it does not ship was the gap; this
# closes it. No /etc/reac-pw: nothing here is host-wide system config, every fact
# reac_conf.h reads is per-user (%%h/.config/reac-pw/), by design (see reac-pw.env.example, and reac-pw.conf.example for the ONE override file).
install -D -m0644 packaging/reac-pw.service %{buildroot}%{_userunitdir}/reac-pw.service
install -D -m0644 packaging/90-reac-pw.preset %{buildroot}%{_userpresetdir}/90-reac-pw.preset
install -D -m0755 packaging/reac-pw-safe-enable.sh %{buildroot}%{_libexecdir}/reac-pw/reac-pw-safe-enable.sh
install -D -m0644 packaging/reac-pw.1 %{buildroot}%{_mandir}/man1/reac-pw.1

%check
# THE NAMESPACE TESTS RUN ONE AT A TIME. Each of them mints a veth pair, a nested network
# namespace and its own PipeWire; meson's default is one process per core, so a dozen of
# them raced for the builder's namespace and PipeWire startup budget and the losers timed
# out -- a flake that looked like the daemon and was the harness. Everything else still
# runs in parallel. tests/netns-tests-are-serial.sh keeps both halves of this honest.
meson test -C _build --no-suite netns --no-suite load
meson test -C _build --no-suite load --suite netns --num-processes 1

%files
%license LICENSE
%doc README.md
# THE EXPERT'S DISCOVERABLE PATH ("fix installation" — operator, 2026-09-17). A fixed
# install has no way to type a segment declaration or an override until these two
# examples are ON THE MACHINE — before this line they existed only in the source
# tarball, never in the RPM. reac-pw itself prints where to look for them (S_NO_OVERRIDES,
# main.c) the first time it runs with no ~/.config/reac-pw/reac-pw.conf.
%doc packaging/reac-pw.conf.example packaging/reac-pw.env.example
# File capabilities, applied by rpm itself (%%caps survives rpm -V / --restore;
# no setcap scriptlet needed):
#
#   cap_net_raw    raw 0x8819 capture/emit without root
#   cap_sys_nice   SCHED_FIFO for the cadence pacer
#   cap_net_admin  the VLAN sub-interfaces the daemon mints and marks -- AND, since
#                  1.0.7, the launch-time pacer, which needs it TWICE. SO_TXTIME is
#                  capability-gated in the kernel (measured: EPERM on both an
#                  AF_PACKET and a UDP socket from uid 0 holding NET_RAW and not
#                  NET_ADMIN), and the daemon installs its own ETF qdisc over
#                  rtnetlink on the device it binds, which is an RTM_NEWQDISC.
#                  Without it the ETF default falls back to the thread backend,
#                  loudly, and publishes reac.pace.backend-refusal -- the desk still
#                  carries audio, with a ~10x looser egress cadence.
#
# The unit's own comments getcap-guard on nothing (systemd cannot read file caps
# before exec), so it is the daemon's own preflight, and openmixer's
# scripts/deploy-live.sh, that refuse a start/restart without them. This stays a
# USER unit: a file capability is what the daemon needs, not root.
%caps(cap_net_raw,cap_net_admin,cap_sys_nice=ep) %{_bindir}/reac-pw
%{_userunitdir}/reac-pw.service
%{_userpresetdir}/90-reac-pw.preset
%{_mandir}/man1/reac-pw.1*
%dir %{_libexecdir}/reac-pw
%{_libexecdir}/reac-pw/reac-pw-safe-enable.sh

%post
# NO %%systemd_user_post, SINCE 1.0.19. That macro expands to
# `systemd-update-helper install-user-units reac-pw.service`, which runs
# `systemctl --no-reload preset --global reac-pw.service` -- `--global`, not scoped to
# whichever user ran `dnf`. Measured 2026-09-18: `sudo dnf install reac-pw-1.0.18` ran
# %%post as root during the transaction, applied 90-reac-pw.preset's `enable
# reac-pw.service` GLOBALLY (a symlink under /etc/systemd/user/default.target.wants/,
# read by EVERY systemd --user instance on the host, present or future), and the very
# next `sudo` invocation that spawned root's own user manager started a SECOND reac-pw
# as root. It won the abstract segment-lock socket the console user's daemon needed,
# which logged "the segment is held: stop the holder" and every box vanished (links
# 206 -> 46) until the root instance was killed by hand.
# This package enables the unit for NOBODY, ever, from a scriptlet. The operator runs,
# once, as the console user (README.md, docs/install/services.md):
#   systemctl --user enable --now reac-pw
# Clean up any global enablement a <=1.0.18 reac-pw left behind on THIS host already --
# harmless (a no-op, exit suppressed) if none exists.
systemctl --global disable --no-warn reac-pw.service >/dev/null 2>&1 || :

%posttrans
# THE SAFE AUTO-ENABLE (#104). %%post above enables the unit for nobody, because a
# scriptlet running as root cannot know who the console user is and `--global` reaches
# every user manager on the host. This does the one thing that is safe instead: when
# there is EXACTLY ONE real interactive login session (logind class `user`, not root),
# enable the unit for THAT user through their own manager --
#   systemctl --machine=<user>@.host --user enable --now reac-pw.service
# -- and in every other case (nobody logged in, several users, root only, linger only)
# touch nothing, leaving the manual step in README.md exactly as it was. It cannot
# enable a session that does not exist yet, which is what the 1.0.19 incident was.
# %%posttrans, not %%post: it runs once, after the whole transaction has settled.
# The decision lives in the helper so it is tested without an rpm transaction
# (tests/safe-enable-selects-one-user.sh); `|| :` because a helper never fails an install.
%{_libexecdir}/reac-pw/reac-pw-safe-enable.sh || :

%preun
%systemd_user_preun reac-pw.service

%postun
%systemd_user_postun reac-pw.service

%changelog
* Thu Oct 08 2026 Pau Aliagas <linuxnow@gmail.com> - 1.0.33-1
- A stagebox with no outputs, like a 40 in / 0 out S-4000S, no longer leaves a
  stereo output on the graph. A build that asked for its playback node at zero
  channels got a two-channel sink named reac-playback and described as "REAC
  segment door (no box recognized yet)"; a zero width now means no playback
  node at all, for a box found on the wire and for one pinned with --box
  alike.
- For such a box, reac-capture carries everything reac-playback carries for a
  box with outputs: the segment's master state, rate, role, discovery, health
  and head-amp properties, and it takes the reac.cfg.rate, reac.cfg.role and
  reac.headamp writes. When a box with outputs gets its playback node back
  after a failed start, reac-capture stops being the door.

* Thu Oct 08 2026 Pau Aliagas <linuxnow@gmail.com> - 1.0.32-1
- reac-pw is now installed from the FreeMixer package channel: signed RPMs for
  Fedora 44 (x86_64, aarch64) and DEBs for Debian bookworm and trixie,
  including Raspberry Pi OS (amd64, arm64). Add the channel once, then install
  or update with dnf or apt. The daemon itself does not change.
- The Debian package installs the same user service as the RPM, gives the
  daemon the file capabilities it needs to run without root, and starts the
  service for the user who is logged in when exactly one user is. With nobody
  or several users logged in it changes nothing, and the one command to run is
  `systemctl --user enable --now reac-pw`.
- reac-pw has a manual page, `man reac-pw`.
- Needs libreac and libreac-transport 1.7.0 or later, from the same channel.

* Wed Oct 07 2026 Pau Aliagas <linuxnow@gmail.com> - 1.0.31-1
- 1.0.30 was tagged but never published: its source tarball lacked
  BUILDING.md, so the package's own checks failed while it was being built.
  1.0.31 is the same code with the tarball fixed, and it carries everything
  listed under 1.0.30.

* Wed Oct 07 2026 Pau Aliagas <linuxnow@gmail.com> - 1.0.30-1
- Every stagebox is now described by what it says on the wire. Its capture and
  playback channels follow the inputs and outputs it declares, its name comes
  from its own identity (S-1608, S-0808, S-4000S-3208, S-4000S-1624 ...), and
  its firmware and hardware block are shown beside it. A box that declares no
  outputs, like a 40 in / 0 out S-4000S, gets no playback node.
- The box's name is published as reac.box-name, next to reac.box-model.
- A box whose family has never been seen is named by its channel counts
  (REAC-0816) and the log asks for a capture of it.
- The built-in model list is no longer used for a connected box. If it
  disagrees with the box, the box wins and the log says "catalogue defect".
- An S-0808 now shows its firmware and hardware block: its identity is asked
  for again after it joins, and its name, sent in two pieces, is read.
- A stagebox in master mode is listened to again on libreac 1.6 and later.
- Needs libreac 1.7.0.

* Tue Oct 06 2026 Pau Aliagas <linuxnow@gmail.com> - 1.0.28-1
- The source tarball carries the protocol's numbers (facts/) and NOTICE.
  1.0.27's tarball had neither and its package build stopped, so 1.0.27 was
  tagged and never packaged; 1.0.28 is 1.0.27's code.

* Tue Oct 06 2026 Pau Aliagas <linuxnow@gmail.com> - 1.0.27-1
- A box that no model row matches gets nodes sized from its declaration: the
  capture node takes the declared inputs, the playback node the declared
  outputs, and the box is named S-4000S-<in><out> unless a Roland row has the
  same widths. A box that declares no outputs (40 in / 0 out) gets no playback
  node.
- A box with no playback node carries its identity on the capture node:
  firmware, REAC version, hardware block and MAC are stamped on both nodes,
  and a capture node rebuilt after a configuration change is stamped at once
  instead of staying blank.
- Built against libreac 1.5.1, which grants a box up to 40 channels wide.

* Thu Sep 24 2026 Pau Aliagas <linuxnow@gmail.com> - 1.0.26-1
- A playback node that fails alone no longer tears down a healthy capture
  node. Recovery judges each side of the pair, and each rebuild line names its
  own node and reason.
- The pacer's fallback from the ETF qdisc says what happened: it reports a
  qdisc as removed only when this daemon installed it, and a failed removal is
  retried at exit.
- The log line about too many VLANs says the extra segments are not served.

* Wed Sep 23 2026 Pau Aliagas <linuxnow@gmail.com> - 1.0.25-1
- A node whose PipeWire server died is no longer taken for one still on the
  graph. After a PipeWire restart the pair is rebuilt, where before a box
  could stay established with no node for the better part of an hour.
- The daemon no longer restarts a wire it is not sending on, and says once per
  change why it holds.
- A declared VLAN is created only after its tag is heard on the trunk; the
  declaration pins the role.

* Tue Sep 22 2026 Pau Aliagas <linuxnow@gmail.com> - 1.0.24-1
- A VLAN heard from any tagged frame is a segment: a trunk's cold VLANs appear
  from the switch's own traffic instead of having to be declared, and the
  journal says "REAC heard on this vid" when the frame arrives. Needs libreac
  and libreac-transport 1.5.0.
- A wire carrying 2000 frames a second is no longer taken for silent while a
  segment is being served, which made the daemon take the master role and
  yield it back.

* Thu Sep 17 2026 Pau Aliagas <linuxnow@gmail.com> - 1.0.19-1
- Installing reac-pw no longer enables the service for every user of the
  machine. A second daemon, started under root's user manager by `sudo dnf
  install`, had won the segment lock and locked the console user out. The
  package now only removes any such leftover from an older install; you enable
  the service once, as the console user, with `systemctl --user enable --now
  reac-pw`.
- The daemon refuses to start as root.
- When the segment is already held, the refusal names the holder's process and
  user where the system allows it.

* Fri Sep 11 2026 Pau Aliagas <linuxnow@gmail.com> - 1.0.0-1
- The PipeWire-native REAC endpoint on libreac and libreac-transport 1.0.0:
  trunk VLAN segments, master and slave at 44.1, 48 and 96 kHz, and head-amp
  control on the wire.
