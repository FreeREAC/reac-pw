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
