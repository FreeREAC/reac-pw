# Building reac-pw

For installing a released package, see the [README](README.md). This page is for building
reac-pw from source, running its tests, and cutting a release.

Target: Fedora + PipeWire 1.4.

## Dependencies

```
sudo dnf install meson ninja-build gcc pipewire-devel libreac-devel libreac-transport-devel
sudo apt install meson pkgconf gcc libpipewire-0.3-dev libspa-0.2-dev libreac-dev libreac-transport-dev
```

The first line is for Fedora, the second for Debian and Raspberry Pi OS; both read the FreeMixer
channel described in the [README](README.md).

`libreac-transport-devel` is a hard requirement (no fallback). `libreac-devel` is resolved
the same way, or meson's wrap fetches and builds it as a subproject when no system package
is new enough. The version floors are the ones `meson.build` declares.

## Build and test

```
meson setup   build
meson compile -C build
meson test    -C build
```

`meson test -C build` runs the unit suite; it needs no PipeWire and no live REAC wire.
The whole-binary tests in the `netns` suite mint unprivileged user, net and pid namespaces
and skip (exit 77) on a machine that cannot; run them one at a time:

```
meson test -C build --suite netns --no-suite load --num-processes 1
```

**Rebuild with `tools/build.sh`, not a bare `meson compile`.** File capabilities live on
the inode and every relink drops them, so a freshly compiled `reac-pw` cannot open its raw
socket. `tools/build.sh [builddir]` compiles, sets
`cap_net_raw,cap_net_admin,cap_sys_nice=ep` and refuses if `getcap` does not show them.

## Against a libreac checkout

When the libreac you need is not released yet, build against a sibling checkout with
nothing installed:

```
tools/build-with-libreac.sh ../libreac [builddir] [-- meson-test-args...]
```

It builds libreac into a static prefix inside the build directory and puts that prefix
first on `PKG_CONFIG_PATH`, so an installed libreac of another version is never picked up.

## The CI suite

`.github/workflows/test.yml` runs `tools/ci-suite.sh` in a privileged `fedora:44`
container, against libreac at the tag the workflow pins (`LIBREAC_REF`). The same script
runs on a desk:

```
tools/ci-suite.sh ../libreac build-ci [../reac-protocol]
```

It builds libreac's `fake_box` (the far end several tests drive), runs the unit suite,
then the `netns` suite, and fails if any namespace test did not report OK. With a
reac-protocol checkout it also builds against perturbed protocol facts
(`tools/facts-perturb-check.sh`).

## Tools

`tools/` carries the on-wire diagnostics used to measure a running daemon without a
rebuild — among them `clock-drift.py` and `ring-depth.sh` (pacer/clock health),
`probe-ports.sh` and `wav-rms.py` (per-port level), and `tone-purity.py` / `sine-level.py`
(signal quality on a captured tone).

## Packages

`packaging/reac-pw.spec` builds the RPM and `debian/` the DEB; both run the unit suite, then the
`netns` suite one test at a time, and install the same user unit, preset, manual page and
`reac-pw-safe-enable.sh`. The release workflow builds the RPM from `git archive` of the tagged
commit, so only committed files reach the package build. The version is the spec's `Version:`,
`meson.build`'s `version:` and the newest entry of `CHANGELOG.md`; a release bumps all three together,
by its last digit.

### Changelog

`CHANGELOG.md` is the one changelog. The spec's `%changelog` and `debian/changelog` are generated
from it with `changelog.sh` of [FreeMixer/.github](https://github.com/FreeMixer/.github)
(`.github/actions/changelog/changelog.sh`), and CI refuses a copy that was edited by hand:

```
changelog.sh sync                # rewrite the spec's %changelog and debian/changelog
changelog.sh check -t vX.Y.Z     # what CI runs; the tag must be the newest entry
```

## Releasing

Add the version's entry to `CHANGELOG.md`, run `changelog.sh sync`, set `Version:` in the spec and
`version:` in `meson.build`, then tag `vX.Y.Z`. The tag runs `.github/workflows/release.yml`, which
calls the shared `build-rpm.yml` and `build-deb.yml` workflows of FreeMixer/.github: signed RPMs for
Fedora 44 (x86_64, aarch64) and DEBs for Debian bookworm and trixie (amd64, arm64) are published to the
FreeMixer channel and attached to the GitHub release, whose notes are the changelog entry. A pull request
or a branch runs the same workflows as a dry run that builds, lints and publishes nothing.

`libreac` and `libreac-transport` are build dependencies taken from the channel, so the libreac
release that `meson.build` asks for is published first.
