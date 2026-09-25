# Lane notes: H3, H1, M6 — 2026-09-25

Follow-up to [the 2026-09-24 review](2026-09-24-reac-pw-review.md). Three lanes, one per
finding, each off `main` at `c3f1408`:

| finding | branch | proof |
|---|---|---|
| H3 | `lane/reac-pw-h3-suite-can-fail` | Sabotage check: `return -1` at the top of `listener_open`. Main: SKIP SKIP, exit 0. Branch: FAIL FAIL, exit 2. Restored: OK OK, exit 0. |
| H1 | `lane/reac-pw-h1-refused-open` | `tests/refused-open-releases-its-engine.sh`. Main: fds 25→29 over five refusals, FAIL. Branch: flat at 24, OK. |
| M6 | `lane/reac-pw-m6-tap-leak` | `tests/vacant-tap-releases-its-ring.sh`. Main: VmData +5124 kB per teardown, FAIL. Branch: flat, OK. |

Every verdict above is meson's own per-test result and its exit status, not a count. No rig
was used, and no PipeWire beyond the private one each test starts.

## What the lanes found beyond the review

**M6 names the wrong resource, in part.** The review says a vacant tap door leaks its RX
*socket* and its ring. The socket does not leak. `reac_rx_open` opens its capture only to
detect the rate and closes it before returning (libreac 1.5.0,
`transport/src/reac_rx.c:487`), and a vacant door never starts the feeder that would open
another. The daemon's fd count is flat on `main` across the teardowns. The ring does leak:
40 ch × pow2(rate/4) floats, 5 MB per teardown at 96 kHz.

**H1's leaked thread does exit, and that is the race.** The slave engine's loop runs while
`s->running` is set, and `hearing_serve`'s memset zeroes that flag under it. The thread then
leaves unjoined, while the next open is already re-initialising the same struct. So the
thread count stays flat on `main` while the engine's socket leaks, one per refusal.

**The libreac subproject fallback is stale.** `subprojects/packagefiles/libreac/meson.build`
declares `version : '0.6.0'` and lists only the pre-1.x sources. Against `meson.build`'s
`>=1.5.0` floor, a checkout with no system libreac fails at configure:
`found 0.6.0 but need: '>=1.5.0'`. The floor is doing its job. The fallback cannot satisfy
it, so the documented dev path is `tools/build-with-libreac.sh`. CI uses that path too.

**Where the netns suite can and cannot run.** Measured in a cloud container: Ubuntu 24.04,
kernel 6.18, PipeWire 1.0.5, WirePlumber 0.4.17.
- **User namespaces and a private PipeWire work there.** 22 of the 30 netns tests ran.
- **The kernel has no `8021q`, `dummy` or `sch_etf`.** The tests that need them now SKIP by
  name: before this lane they read `rc=90` as SKIP, or as an unexplained FAIL.
- **`box-master-slave-join.sh` fails there:** "reac-playback.bmx0 has 0 input ports". Its
  WirePlumber drop-in uses 0.5's `wireplumber.profiles` syntax, which 0.4 ignores. This is
  why `.github/workflows/test.yml` runs the suite in `fedora:44`, the release's own target,
  and not on the Ubuntu runner directly. It is still to be confirmed green there on the
  workflow's first run.
