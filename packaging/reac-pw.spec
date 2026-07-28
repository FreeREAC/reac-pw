# SPDX-License-Identifier: GPL-3.0-or-later
# reac-pw — PipeWire-native REAC endpoint, for the Fedora MiniPC target.
Name:           reac-pw
# Overridable at build time -- the tarball/CI wrapper passes
#   --define "version_override $(git describe --tags ...)"
# so releases version from git tags; the fallback tracks meson.build's version.
Version:        %{?version_override}%{!?version_override:0.1.0}
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
BuildRequires:  pkgconfig(libreac) >= 0.3.0
Requires:       pipewire

%description
reac-pw exposes a Roland REAC stream as PipeWire graph nodes: reac:capture
decodes the master's 40-channel downstream broadcast into mono DSP sources, and
reac:playback encodes graph audio back onto the wire (EtherType 0x8819). It also
carries the virtual-stagebox JOIN/HOLD connection FSM so the node can present
local inputs to a real Roland master. Built for a Fedora MiniPC running a
PREEMPT_RT kernel + PipeWire.

Links dynamically against the system libreac (>= 0.3.0), which carries the
shared REAC byte-layout core: frame validation, 24-bit decode (incl. the braid
codec, the f32<->s24 sample pair and the box-upstream decode), the OHRCA +2
trailer rule, capture and pcap replay. Nothing is vendored.

%prep
%autosetup -n %{name}-%{version}

%build
# Explicit meson (the host uses a pip-installed meson, not the dnf macros).
# %%set_build_flags exports the Fedora CFLAGS/LDFLAGS (incl. -g and the linker
# build-id) so the plain buildtype still yields a real debuginfo package.
# --wrap-mode=nofallback: the libreac dependency MUST resolve to the system
# libreac-devel (pkg-config), never the bundled subproject wrap -- the RPM links
# libreac dynamically (runtime dep auto-generated from the libreac.so.0 soname).
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
%caps(cap_net_raw,cap_sys_nice=ep) %{_bindir}/reac-pw

%changelog
* Sun Jun 14 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.0-1
- Initial package: PipeWire-native REAC endpoint for the Fedora MiniPC.
