# Environment knobs

Every `REACPW_*` / `REAC_*` environment variable read by reac-pw, one line
each. The contract for all of them: **unset = default behavior, byte-identical
on the wire** — a knob whose motivating theory is debunked gets deleted, not
kept around (see the repo discipline; the scene-commit / announce A/B knobs
and `REAC_TX_LAYOUT` were removed under that rule once their hypotheses were
resolved).

| Knob | Effect | Default |
|---|---|---|
| `REACPW_GRANT_DWELL_S` | Master role: hold the recognized-but-ungranted dwell (ENROLL -> grant burst) for N whole seconds. A real M-200 holds a cold box ungranted ~27 s while it climbs its JOIN field; the built-in dwell is ~1.6 s (`REAC_M_GRANT_DWELL_SECONDS_X10`). Only the dwell LENGTH changes — the FSM sequence is untouched. | unset (built-in ~1.6 s) |
| `REAC_DEBUG` | Opt-in diagnostic telemetry on stderr, ~every 2 s: RX feeder counters (ok/dup/other/bad/gaps, locked box MAC) in `reac_rx.c` and source-node ring stats (active channels, peak, fill) in `reac_source_node.c`. Set to any value to enable. | unset (silent) |

Campaign/measurement knobs used during rig RE sessions live on their campaign
branches, not on `main` (e.g. the head-amp no-enroll campaign's
`REACPW_NO_ENROLL` / `REACPW_OP0100_BURST` on `wip/headamp-noenroll-campaign`)
— they are documented in the branch's own commits and get folded into code
defaults or deleted when the protocol question closes.

Both knobs above are also listed in `reac-pw --help` (the `environment`
section of `usage()` in `src/main.c`). Keep the three places in sync when a
knob is added or removed: the code site, `usage()`, and this file.
