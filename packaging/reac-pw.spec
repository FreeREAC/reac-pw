# SPDX-License-Identifier: GPL-3.0-or-later
# reac-pw — PipeWire-native REAC endpoint, for the Fedora MiniPC target.
%global debug_package %{nil}
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
BuildRequires:  pkgconfig(libreac)
BuildRequires:  systemd-rpm-macros
Requires:       pipewire

%description
reac-pw exposes a Roland REAC stream as PipeWire graph nodes: reac:capture
decodes the master's 40-channel downstream broadcast into mono DSP sources, and
reac:playback encodes graph audio back onto the wire (EtherType 0x8819). It also
carries the virtual-stagebox JOIN/HOLD connection FSM so the node can present
local inputs to a real Roland master. Built for a Fedora MiniPC running a
PREEMPT_RT kernel + PipeWire.

The source tarball vendors libreac and the reac-aes67 decode core so the build
is self-contained (no network fetch).

%prep
%autosetup -n %{name}-%{version}

%build
# Explicit meson (the host uses a pip-installed meson, not the dnf macros).
meson setup _build --prefix=%{_prefix} --buildtype=plain \
      -Dreac_aes67=third_party/reac-aes67-core
meson compile -C _build

%install
DESTDIR=%{buildroot} meson install -C _build
install -Dm0644 packaging/reac-pw.service %{buildroot}%{_unitdir}/reac-pw.service
install -Dm0644 packaging/reac-pw.conf     %{buildroot}%{_sysconfdir}/reac-pw/reac-pw.conf

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
%{_unitdir}/reac-pw.service
%dir %{_sysconfdir}/reac-pw
%config(noreplace) %{_sysconfdir}/reac-pw/reac-pw.conf

%post
%systemd_post reac-pw.service

%preun
%systemd_preun reac-pw.service

%postun
%systemd_postun_with_restart reac-pw.service

%changelog
* Sun Jun 14 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.0-1
- Initial package: PipeWire-native REAC endpoint for the Fedora MiniPC.
