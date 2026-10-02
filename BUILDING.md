# Building reac-pw

For installing a released package, see the [README](README.md). This page is for building
reac-pw from source, running its tests, and cutting a release.

Target: Fedora + PipeWire 1.4.

## Dependencies

```
sudo dnf install meson ninja-build gcc pipewire-devel libreac-devel libreac-transport-devel
```

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

## RPMs

`packaging/build-rpm.sh` builds the RPMs locally from `packaging/reac-pw.spec` (the spec's
`%check` runs the unit suite).

## Releasing

`.github/workflows/release-rpm.yml` builds the reac-pw RPM in a `fedora:44` container from
`packaging/reac-pw.spec` and publishes it into the public dnf tree at
[freereac.github.io/rpm](https://freereac.github.io/rpm) (one repo, `freereac.repo`, one GPG
key) — the same tree libreac's own release-rpm.yml publishes into beside it. It is
`workflow_dispatch` only, never on push:

```
gh workflow run release-rpm.yml -f tag=v1.0.1 -f sign=false   # dry run, publishes nothing
gh workflow run release-rpm.yml -f tag=v1.0.1 -f sign=true    # signs and publishes to the public dnf tree
```

`tag` must already exist and match `v[0-9]*`. `sign` defaults to `false`, which stops
before the tree is touched — the assembled unsigned tree is still attached to the run as an
artifact for inspection. Building needs `pkgconfig(libreac)` and
`pkgconfig(libreac-transport)` at the spec's floors, resolved from the same public tree by
installing its `freereac.repo` before `dnf builddep` runs — so **libreac's own equivalent
workflow must have published there first**, or the build fails loudly and by name.

**Hand-publish fallback**, if the workflow cannot run (no runner, a secret missing): build
locally and run `packaging/publish-repo.sh --rpm-dir DIR --out <checkout of
freereac.github.io> --key-id A14B3E1E1F69EBF4`, then commit and push `rpm/` from that
checkout — the same script the workflow calls, run by hand over the same tree.
