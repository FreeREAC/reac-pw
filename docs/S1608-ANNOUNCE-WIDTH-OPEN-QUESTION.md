# The S-1608 announce-width divergence — investigated, not fixed

Workstream F's wire differential (82 captures, 2026-08-22) found reac-pw's cfea
announce (`out[18]`, `gen_cfea` in `src/reac_master.c`) sometimes carries a
width byte other than `0x10` (16) while courting or holding an S-1608, where
real desks show `0x10` throughout. This note records what was checked before
concluding the fix is not safe to guess at tonight.

## Where the byte comes from

`gen_cfea()` writes `out[18] = cfg->out_channels`. That field is stamped in
exactly two places:

- `reac_master_set_box()` — the moment the box's config-announce is parsed
  (`reac_pacer_rx_ingest`), stamps the box's TRUE declared width (always a
  clean multiple of 4 — `reac_ports_parse` counts 4-channel port-table slots,
  so 16 for an S-1608, never anything else).
- `enter_granting()` — on a cold-connect JOIN (which carries no width),
  `cfg->out_channels` is left UNTOUCHED if `!reac_master_has_box(m)`. Since a
  fresh master or one that just forgot a box holds the IDLE placeholder
  (`REAC_CONSOLE_CFG_IDLE.out_channels = 8` — see its own comment: *"the value
  every captured desk announces while unlinked"*, evidence-backed), the
  announce reads `0x08` from the moment the JOIN opens GRANTING until the
  box's config-announce is parsed and `reac_master_set_box` stamps the real
  width.

## What this rules out

`0x07` cannot come from either path: `reac_ports_parse` only ever produces a
multiple of 4 (`in_slots * REAC_PORTS_CH_PER_SLOT`), the S-1608's matrix row
(`BOX_MODELS[0]`, `reac_ctrl.c`) declares `in_ch = 16` correctly, and
`reac_grant_allocate`/`set_enroll_width` pass `in_ch` through unchanged. No
code path in reac-pw's master role can compute 7 from a correctly-parsed
S-1608 declaration. Either the capture tooling is reading a different byte
than intended, or the 7 comes from a state this note did not reach (a partial
frame during the RX gate, a checksum-failed frame not making it to
`reac_master_set_box` at all, or something outside this repo).

## The one real candidate: the JOIN-to-CONFIG gap

`0x08` IS explained: it is the idle placeholder, held on the wire for however
long a real S-1608 takes between its JOIN and its config-announce arriving.
The evidence trail (the `REAC_CONSOLE_CFG_IDLE` comment) only establishes that
`0x08` is correct while NO box is linked at all — it says nothing about
whether a real M-200 shows `0x08` or something else during THIS specific
in-between window, because nobody has captured that window in isolation.

Two readings of the same fact, and this session could not tell them apart:

1. **It is not a real divergence.** `box_count` is 0 throughout this window
   (the RECOGNIZED-BUT-UNGRANTED state stamps `count=0` regardless of width),
   and a receiver that reads count before width would treat the byte as
   don't-care while count is 0 — the same way it is don't-care during the
   fully-idle case the evidence already covers. F's differential, reading raw
   bytes without that semantic, would flag it anyway.
2. **It is real**, because a real M-200 already knows a specific box's model
   before its own JOIN response — from a prior sighting, a per-model default,
   or simply because its own JOIN-to-CONFIG gap is short enough (or its
   capture cadence coarse enough) that `0x08` never lands in a captured frame.

## Why no fix landed

Closing this requires knowing what byte a REAL M-200 shows in this specific
window — captures of the fully-idle case and the fully-established case exist,
this window's does not. A structural fix (pre-seeding `cfg.out_channels` from
passive discovery before our own JOIN completes, or suppressing the width byte
entirely while `count == 0`) touches FSM timing on the handshake that gets the
box ESTABLISHED at all, with no way to verify safety without rig access — and
tonight's mandate is explicitly no rig access, no deploy, no restart.

**The open question for whoever has the raw captures**: across the 11 real
S-1608 sessions, what does `cfea out[18]` read in the frames between the box's
JOIN and its first config-announce, and does `box_count` in those same frames
ever read anything but 0? If count is always 0 there, this is likely finding
(1) above and needs no code change. If a real desk shows `0x10` even there, it
is finding (2), and the fix is almost certainly threading the box's identity
in earlier than `reac_master_set_box` currently does — a change that should
land with its own capture-backed test, not a guess.
