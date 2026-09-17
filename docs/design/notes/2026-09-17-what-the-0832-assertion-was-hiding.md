# What the 0832 assertion was hiding

Status: MEASUREMENT — no ruling. Recorded because the finding that fixed the assertion
turned a silent arm into a red one, and the red is real.

`tests/box-0832-enrols.sh` asserted `recognized box = S-4000S-0832`, a string no code path
in this tree emits (the announcement is `autodetected <display>`). The review of PR #103
named it; the assertion now reads the daemon's own line.

Run on the desk, 2026-09-17, reac-pw at 3ebca7d and at this lane's head, against
libreac's `fake_box` wearing the `s4000s-0832` row (system libreac 1.2.1):

```
reac-pw: autodetected S-4000S-3208 (32 in / 8 out) -> reac-capture 32 in / reac-playback 8 out
reac.box-model: s4000s          reac.box-width: 32x8
```

**The split chassis is recognised as the 32/8 row.** Arm 1 therefore fails twice — the
name and the published width — and arm 2, the 32/8 control, passes both with the same
greps, so the instrument is sound and the two rows are told apart.

This is NOT caused by the review lane: the same two failures appear with the 3ebca7d
binary and the 3ebca7d test script (which could only ever fail on its phantom string).
What the lane changed is that the failure now names the row the daemon actually chose.

Open, for whoever takes it: the 1.0.15 changelog says the S-4000H-0832 enrols, and on the
rig it did. What the offline harness shows is a `fake_box` declaration that resolves to
`s4000s` here — either the fake's declaration is not the captured one, or the width-keyed
resolution answers the first row of that width (the spec's own amendment warns about
exactly that: "a width never names this row"). Measure before ruling.

## ANSWERED, same day: the far end was not the box the test asked for

Status: MEASURED. The red above is real and it is the HARNESS's, not the daemon's.

`fake_box <if> <secs> <token>` only grew its third argument with the 0832 row (libreac
1.2.1). An older binary takes the token, ignores it, and declares its built-in
`COMMIT_REPORT` — the capture's S-4000S-3208, byte for byte. That is what the wire carried:

```
$ strings ~/Devel/audio/libreac/fake_box  | grep -c 'declaring as'     # built 02:34, pre-token
0
$ strings ~/Devel/audio/libreac-wt-0832h/fake_box | grep -c 'declaring as'
1
```

With a `fake_box` from libreac 1.2.1 (24db957), the same script and a reac-pw built at
8dad76e both arms pass on the desk:

```
fake_box: declaring as S-4000S-0832 (8 in / 32 out) — 8 in / 32 out, upstream 8 channels (340 B)
reac-pw: autodetected S-4000S-0832 (8 in / 32 out) -> reac-capture 8 in / reac-playback 32 out
    reac.box-model: s4000s-0832       reac.box-width: 8x32
fake_box: declaring as S-4000S-3208 (32 in / 8 out) — 32 in / 8 out, upstream 32 channels (1204 B)
reac-pw: autodetected S-4000S-3208 (32 in / 8 out) -> reac-capture 32 in / reac-playback 8 out
```

Nothing was wrong with either side of the wire, and both were checked before the harness
was: the row's synthesised config-announce is byte-identical to the captured block
(`01 03 00 10 84 00 00 00 | 01×8 00 00 03 03 | tail | 56`), `reac_ctrl_identify_box` on a
frame built the way `fake_box` builds it answers `s4000s-0832`, and `reac_ports_parse`
reads 8 in / 32 out, headamp base 0.

**The rule it cost: a probe that cannot say WHICH box it is pretending to be cannot
testify about any box.** Both arms now require `fake_box`'s own `declaring as <row>` line
before they read a single word of the daemon's — the same positive control the injected
tone needed on the console (CLAUDE.md, 2026-08-13). The stale binary now names itself.
