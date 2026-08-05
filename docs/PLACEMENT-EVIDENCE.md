# Fabric-slot PLACEMENT — what 82 captures actually show

**Status:** evidence study, 2026-07-28 (issue #210). Verdict: **the placement base is
not yet a derived law — but it is no longer "three data points", and every alternative
we could name is now dead except three that the corpus cannot separate.**

This document replaces the "three points across two desk models" claim in
`src/reac_grant.c`. It is the corpus-wide answer to: *why is an S-1608 addressed at
`0x20` when an S-0808 and an S-4000S are both addressed at `0x00`?*

Analyzer + raw rows (private repo, not published): `reac-captures/analysis/` —
`reac_pcap.py` (streaming pcap reader + REAC control decoder), `placement_scan.py`
(one JSON row per capture), `make_table.py` → `placement_table.csv`.

## Corpus coverage

| | captures | note |
|---|---|---|
| scanned | **82** | 47 in `reac-captures/captures/` + 35 in `m200-headamp-re/`, `m200-s1608-headamp/`, `m200-scene-recall-re/`; ~19 GB, streamed |
| carry a **grant sweep** (a usable base+span tuple) | **42** | the master's `1212`/TAG `0101` group-A run |
| no sweep in-window | 40 | steady-state / probe-only / MAC-sanitised taps (`zoneA`, `zoneB`, `wired-*`, `real_reac_stream`, the `enrol-0N` slices). They still contribute cfea, chanmap and config-announce rows — they contain no placement decision to observe |

Desks in the corpus: M-200i `c9:cc:03`, M-300 `c9:d8:5b`, M-5000 `ca:15:4c` (+ reac-pw's
own master stand-in `c9:cc:04`). Boxes: S-0808 `c4:dc:9c`, S-1608 `c4:80:3b`, two
S-4000S units `c4:06:80` / `c4:08:bc`, and reac-pw's slave stand-in `c4:80:41`.
**No M-480, no box wider than 32, no fabric other than 48 head-amp slots / 40 audio
slots exists on tape** — so the "wider cases" cannot be tested at all here.

## The measured table (all 42 usable rows collapse to five distinct rows)

| box (declared) | width | cfg selector | cfg byte[7] | ENROLL map | grant base | span | rows |
|---|---|---|---|---|---|---|---|
| S-0808  | 8  | `0x84` | `0x00` | `1i/4o` | **`0x00`** | 8  | 11 |
| S-1608  | 16 | `0x82` | `0x02` | *none ever sent* | **`0x20`** | 16 | 12 |
| S-4000S | 32 | `0x84` | `0x00` | `4i/1o` | **`0x00`** | 32 | 5 |
| reac-pw slave declaring the S-1608 block | 16 | `0x82` | `0x02` | (8-wide default) | **`0x20`** | 16 | 9 |
| partial / aborted sweeps (same bases) | | | | | as above | 6–16 | 5 |

Every sweep is **contiguous**. No row anywhere contradicts another: the same box gets
the same base every single time.

### The decoded fields this study nails down

- **Box config-announce `cdea 01 03 0010`** (cdea-relative offsets):
  `[6]` selector (`0x82` S-1608 / `0x84` S-0808 + S-4000S), `[7:9]` `00 00`,
  `[9]` a one-byte field (`0x02` S-1608 / `0x00` the others), then **12 cells at
  `[10:22]`, 4 channels each = 48 slots**, `0x02` = analog input, `0x01` = output,
  `0x03` = absent. Cell counts reproduce every box exactly: S-0808 2i+2o, S-1608 4i+2o,
  S-4000S 8i+2o (all three boxes have 8 outputs). The cells are an **inventory in
  declaration order, always starting at cell 0** — they are *not* a fabric map.
- **ENROLL group map `cdea 01 03 000d`**: input region `[9:14]` = `width/8` bytes of
  `0x41` front-packed, output region `[14:19]` = the rest as `0xc3` back-packed. A pure
  function of width, byte-identical across M-200/M-300/M-5000 but for the console-model
  byte `[8]`. Confirms `m200-s4000-width-re/FINDINGS.md`.
- **CHANMAP `cdea 01 03 0019`**: the same full 49-position ring (slots `0x00..0x2f`
  valued `0x28`, high bank `0x28..0x2f` valued `0x38`, plus the `0xfe` marker) for
  **every** box on **every** desk. It carries no per-box placement.

## Candidate laws, and how each died

| candidate | prediction | verdict |
|---|---|---|
| **lowest-fit** ("width-many contiguous slots wherever they fit") | 16-wide → `0x00` | **DEAD** — 12 S-1608 rows say `0x20` |
| **top-aligned to the `0x2f` ceiling** | 8-wide → `0x28`, 32-wide → `0x10` | **DEAD** — 16 rows say `0x00` |
| **`f(desk model)` / desk-side allocator** | base varies by desk | **DEAD** — M-200i, M-300 **and** M-5000 each grant the *same* S-1608 `0x20` and the *same* S-0808 `0x00`. `ctl2.pcap` has ONE M-200i granting `0x00` to the S-0808 and `0x20` to a 16-wide box in the same session |
| **`f(enrolment order)` / next-free allocation** | base drifts across re-joins | **DEAD** — `s0808-reboot-enrollfix` holds 22 consecutive re-establishments, every one `0x00`; `reacpw-reconnect` re-grants repeatedly, every one `0x20`. Zero drift in 42 sweeps |
| **`f(box MAC / per-unit identity)`** | base keyed to the unit | **DEAD** — reac-pw's slave on a *different* MAC (`c4:80:41`) that merely **declares** the S-1608's blocks is granted `0x20` by a real M-200 (9 rows). Two different S-4000S units both get `0x00` |
| **base is in the box's CONFIG cell map** | S-1608 cells would start at cell 8 | **DEAD** — all three boxes declare inputs in cells `0..n-1`; the S-1608 declares cells 0–3 yet is placed at `0x20` |
| **`f(ENROLL group map)`** | the map would encode the base | **DEAD** — the map is width-only and front-packed from group 0; and **17 of 18 real-S-1608 captures contain no `0103 000d` at all**, yet the box is still placed at `0x20` |
| **`f(CHANMAP)`** | the map would mark the box's slots | **DEAD** — byte-identical full-ring map for every box |
| **`f(JOIN` / cold-connect identity`)`** | model code drives it | **NOT SEPARABLE** — the identity records (`1212`/TAG `0500`, oplen 22/26) do carry a model code (`…01 00…` S-0808, `…02 02…` S-1608, `…02 05…` S-4000S), perfectly collinear with everything below |

### What survives — three collinear carriers

Across all 42 rows these three are **perfectly correlated**, so the corpus cannot
choose between them:

1. **declared input WIDTH** — 16 → `0x20`, 8 and 32 → `0x00` (this is today's
   `OBSERVED_PLACEMENT`, and it is *observationally correct on every row*);
2. **config-announce selector `[6]`** — `0x82` → `0x20`, `0x84` → `0x00`;
3. **config-announce byte `[9]`** — `0x02` → `0x20`, `0x00` → `0x00`; note
   `base == byte[9] * 0x10` holds arithmetically on every row, which makes this the
   most law-shaped of the three (a 16-slot "unit offset" the box declares).

The trap the original three-point table fell into is still open, just narrower: **we
have three declaration variants, not three boxes.** Any law that separates these needs
a *fourth* variant, and no real box we own supplies one.

## The experiment that settles it

reac-pw's slave role already carries all three real declaration blocks
(`src/reac_ctrl.c` `BOX_MODELS`), so the fourth variant is a one-byte patch, not new
hardware. Run each against the real M-200 on the rig, capture the establishment, and
read the group-A base out of `placement_scan.py`:

| run | reac-pw slave declares | if base is… |
|---|---|---|
| 1 (control) | `--box-model s1608` unmodified | `0x20` — confirms the rig reproduces the corpus |
| 2 | s1608 block, selector `[6]` `0x82`→`0x84`, re-checksummed | `0x00` ⇒ carrier is the **selector**; `0x20` ⇒ it is not |
| 3 | s1608 block, byte `[9]` `0x02`→`0x00`, re-checksummed | `0x00` ⇒ carrier is **byte[9]** (and `base = byte[9]*0x10` is the law) |
| 4 | s0808 block with selector→`0x82` **and** byte[9]→`0x02` | `0x20` ⇒ the two model bytes carry it and **width does not** |
| 5 (control) | `--box-model s0808` unmodified | `0x00` |

If runs 2–4 all leave the base unmoved, the carrier is the declared **width** and
today's table *is* the law — write it as `base = (width == 16) ? 0x20 : 0x00` only once
that is shown, not before.

Two mechanical notes for whoever runs it: `reac_ctrl_build_config_announce` picks the
block by `in_ch` (`reac_box_model_by_channels`), so width and declaration cannot be
decoupled today without a patched build; and `reac_ctrl_checksum_apply` already
re-stamps the block, so a changed byte stays checksum-valid.

## One correction this study forces — RESOLVED IN CODE (#69)

`src/reac_grant.c` reasoned about a "`0x2f` fabric ceiling". That ceiling is real but it
belongs to the **head-amp / chanmap channel space (48 slots, `0x00..0x2f`)**, which is
*not* the audio fabric: cfea advertises 40 audio slots (`[17] = 0x28`) and the ENROLL
group map spans exactly those 40 (5 groups × 8). The S-1608's group-A run reaches
`0x2f` = 47, past 40 — so group-A CH cannot be an audio fabric slot index. Conflating
the two spaces is the 40-vs-48 mismatch already flagged in
`reac-firmware-re/MULTI-BOX-DESIGN.md`; multi-box allocation (#129) must keep them
separate.

**Landed 2026-07-28 (#69).** `REAC_GRANT_FABRIC_CEILING` — named after the audio
fabric while measuring the head-amp space — is gone. Both spaces are now defined
once, with these citations, in `src/reac_slots.h`:

| constant | value | space | evidence |
|---|---|---|---|
| `REAC_AUDIO_FABRIC_SLOTS` / `_CEILING` | 40 / 39 | AUDIO | cfea `[17] = 0x28`; the ENROLL group map's 5 × 8; libreac `REAC_MAX_CHANNELS` |
| `REAC_HEADAMP_SLOTS` / `_CEILING` | 48 / `0x2f` | HEAD-AMP | S-1608 at base `0x20`, 16 wide → group-A run to `0x2f` = 47 |
| `REAC_HEADAMP_RING` | 49 | HEAD-AMP | the 48 chanmap positions + the `0xfe` section marker |

Every site now takes the space it belongs to: the grant allocator and
`reac_grant_alloc_fits` are HEAD-AMP (their bound is the CH the sweep addresses, so
`0x20 + 16 = 0x2f` must stay legal); `reac_ctrl.h`'s `REAC_HEADAMP_MAX_CH` and
`reac_master.h`'s chanmap ring (renamed `REAC_M_FABRIC_RING` → `REAC_M_CHANMAP_RING`)
are HEAD-AMP; `reac_boxreg`'s fabric is AUDIO and must never be widened to 48.
`tests/test_reac_grant.c` §5 pins both directions on the one span where they must
disagree — base 32 × width 16 is head-amp-legal through CH 47 and audio-illegal.

**No bytes moved.** The head-amp ceiling kept its `0x2f` value and the audio fabric
its 40, so this is a naming/binding correction: every golden, fixture and assertion
is untouched and the full suite stayed green across each commit.

## Verdict

**OUTCOME C — still undetermined, deliberately.** No change to the PLACEMENT LAW
(the slot-space split above is a separate, landed correction — it renames and rebinds
constants without moving a byte): the current
`OBSERVED_PLACEMENT` table predicts all 42 usable rows correctly, so there is nothing
to fix, and replacing it with any of the three candidate laws would be picking one of
three indistinguishable hypotheses and calling it knowledge. What changed is the
evidence behind it: it is a five-row, 42-establishment, three-desk, four-unit result,
and the base is proven to be a deterministic function of **what the box declares at
cold-connect** — not of the desk, not of the MAC, not of enrolment order, not of the
enroll map or the chanmap.
