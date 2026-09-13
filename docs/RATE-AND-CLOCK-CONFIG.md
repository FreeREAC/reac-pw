# Rate and clock configuration

reac-pw owns the REAC sample rate and the clock for the segments it masters;
openmixer (or any other console) is a client of that, never a source of it.

## The rate

On a Roland desk the operator picks the REAC rate from a menu; the desk drives the
segment at that rate and every stagebox locks to it — a box has no rate of its
own. reac-pw is the master, so `--rate` / `REAC_RATE` is that menu. As a slave,
the rate is auto-detected from the wire cadence instead, because a slave joins a
segment somebody else is already driving.

## The clock

The rate picks the nominal frame period; the clock is what that period is
disciplined to — a NIC PHC, the PipeWire graph clock when hardware drives it, a
stagebox fed from a house word clock, or nothing. `REACPW_CLOCK_FOLLOW` and
`REACPW_CLOCK_REF` (see [ENV-KNOBS.md](ENV-KNOBS.md)) configure it, per segment,
through the same layered lookup as every other key. One clock master per
segment; boxes follow it.

## Configuration is per segment

One daemon can run several segments (auto-spine); each reads its own settings
from `~/.config/reac-pw/reac-pw.env`, keyed by its interface name
(`REAC_RATE_<segment>`, `REAC_ROLE_<segment>`, ...).

## Precedence, highest first

| # | layer | what it is for |
|---|---|---|
| 1 | the command line (`--rate`, `--live`, ...) | an explicit argument for this invocation |
| 2 | the process environment (`REAC_RATE`, `REACPW_*`) | an operator's override for one run, or what `EnvironmentFile=` delivers to the systemd unit |
| 3 | `<KEY>_<segment>` (e.g. `REAC_RATE_enp131s0`) | per-segment — outranks the bare key in every layer below, because segments can differ |
| 4 | `~/.config/reac-pw/reac-pw.env` | per-host: what every segment on this host shares |
| 5 | `~/.config/openmixer/reac.env` | the last resort — makes a standalone install work with no console present; a console overrides it and shows the operator the result |
| 6 | the built-in default (`REAC_MASTER_DEFAULT_RATE` = 96000) | reached only when every layer above is silent |

An empty value (`REAC_RATE=`) sets nothing and falls through to the next layer. A
layer that answers with nonsense is named and skipped, not silently dropped
(`ignoring REAC_RATE='999' from the process environment`).

The startup line names the layer that won, for example:

```
reac-pw: REAC rate = 48000 Hz (4000 pps), from the command line
reac-pw: REAC rate = 96000 Hz (8000 pps), from the built-in default
```
