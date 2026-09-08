# Where the rate and the clock are configured, and why it is not openmixer

**omx is just the UI.** reac-pw owns the clock and the rate; openmixer is the
control surface and nothing more. Everything below follows from that one line,
and the current rig contradicts it in three places at once.

## The three sources that disagree today

| source | says | who reads it |
|---|---|---|
| the running masters (hand-started in a tmux scope) | `--rate 48000` | the kernel, twice |
| `~/.config/openmixer/reac.env` | `REAC_RATE=96000` | `reac-pw-master.service` — **installed but disabled**, so: nobody |
| `reac-pw --rate`'s own default | **96000** in the master role (was: auto-detect) | reac-pw, when nothing else speaks |

**CORRECTION, and it is mine to own — TWICE.** This file first read
`REAC_RATE=96000` as a stale value and a loaded gun on a 48 kHz rig. Wrong:
**operator ruling, 2026-08-23, "96k is 96kHz and should be the default reac clock
rate."** Then, having been corrected, it went on to recommend DELETING
`~/.config/openmixer/reac.env` as a second ledger. Also wrong, and wrong for a
more interesting reason.

**Operator ruling, verbatim: "We had a layered config and this is the last
resource. Useful for standalone install, but omx needs to override and give the
config to the user."**

Two files carrying one key is a second ledger only when they sit at the SAME
level. These do not. `reac.env` is the BOTTOM LAYER — the last-resort default
that makes reac-pw work on a standalone install with no console present, which
openmixer then overrides and surfaces to the user. **A layer is not a duplicate.**

The defect a layered config actually has is a different one, and this rig had it:
**an override order nobody wrote down.** Then every reader infers a different one
and they are all sure they are right — which is exactly how a morning went on
three sources of one number. So the order is declared below, declared again in
`src/reac_conf.h` where it is implemented, and pinned by `tests/test_reac_conf.c`
so the declaration and the code cannot drift apart.

## The law

1. **The rate belongs to the master, per segment.** On a Roland desk the operator
   picks the REAC rate from a menu; the desk drives the segment at that rate and
   every stagebox locks to it — a box has no rate setting of its own. reac-pw is
   the master, so `--rate` is that menu. `src/main.c` already says this correctly;
   only the configuration around it disagrees.

2. **The clock belongs to the master too, and it is NOT the rate.** The rate picks
   the nominal frame period. The clock is what that period is disciplined TO — a
   NIC PHC, the graph clock when hardware drives it, a stagebox that is itself
   fed from a house word clock, or nothing. `reac_clock.h` carries the full
   hierarchy. One clock master per segment; boxes follow.

3. **Configuration is PER SEGMENT, because a master is per segment.** One file per
   interface, named by the interface. Two segments means two files — but ONE unit
   (auto-spine §5, 2026-08-20-reac-auto-spine.md: a single daemon manages every
   box, spawning an internal listener per interface; a unit-per-segment shape was
   proposed here and REJECTED there). Any shape that cannot say "one file per
   segment" is the shape that ends up hand-started in tmux — which is exactly
   where the rig was before the service existed.

4. **THE PRECEDENCE, HIGHEST FIRST. This is the law.**

   | # | layer | what it is for |
   |---|---|---|
   | 1 | **the command line** (`--rate`, `--live`, ...) | an explicit argument. **This is the layer openmixer uses** — the console owns the desk's configuration and hands it over when it launches us, which is what "omx needs to override" means in practice |
   | 2 | **the process environment** (`REAC_RATE`, `REACPW_*`) | an operator's ad-hoc override for one run, and the channel systemd's `EnvironmentFile=` delivers on. Above the files because a variable set for THIS invocation is more specific than a file describing every one |
   | 3 | **`<KEY>_<segment>`** — `REAC_RATE_enp131s0` | **per-segment.** The rig has two segments and they are not interchangeable — different boxes, different NICs, potentially different rates. A per-segment fact is the key suffixed with the segment's name (its interface), in any of the layers below, and it outranks the bare key in every one of them — even the environment, because systemd's `EnvironmentFile=` exports the whole of `reac-pw.env` and a bare key there is the same file speaking. Segments are discovered, not declared (the trunk-VLAN spec, amendment 2026-09-02), so there is no per-segment FILE |
   | 4 | **`~/.config/reac-pw/reac-pw.env`** | per-host: what every segment on this host shares |
   | 5 | **`~/.config/openmixer/reac.env`** | **the last resort, and it STAYS.** What makes a standalone install work with no console present. openmixer overrides it from above and shows the user the result |
   | 6 | **the built-in default** (`REAC_MASTER_DEFAULT_RATE` = 96000) | compiled in; reached only when all five above are silent |

   **An empty value is not an answer.** `REAC_RATE=` sets nothing and falls
   through to the next layer, because a key someone blanked out is a key they
   turned off, not a key they set to the empty string.

   **A layer that answers with nonsense is named and skipped**, not silently
   dropped: `ignoring REAC_RATE='999' from the process environment`. Otherwise a
   config file gets blamed for working and a default gets blamed for not.

5. **Auto-detect is a SLAVE's default and is wrong for a master.** A slave joins a
   segment somebody else is already driving, so detecting the rate is the only
   thing it can do. A master DEFINES the rate: on a silent segment there is
   nothing to detect, and "auto" resolved to whatever the fallback happened to be
   with nothing on screen saying which.

6. **THE STARTUP LINE NAMES THE LAYER THAT WON, not just the value.** A layered
   config that cannot tell you which layer answered is a debugging trap. Measured
   on this host, every layer, in one sitting:

   ```
   reac-pw: REAC rate = 48000 Hz (4000 pps), from the command line
   reac-pw: REAC rate = 44100 Hz (3675 pps), from the process environment
   reac-pw: REAC rate = 48000 Hz (4000 pps), from a per-segment key (<KEY>_<segment>) in the environment or a conf file
   reac-pw: REAC rate = 96000 Hz (8000 pps), from ~/.config/openmixer/reac.env (last resort)
   reac-pw: REAC rate = 96000 Hz (8000 pps), from the built-in default
   ```

## Does the code implement the law? IT DOES NOW — IT DID NOT BEFORE

Asked to verify rather than assume, and the answer was no, in three places:

- **reac-pw read no configuration file at all.** It consulted `argv` and a handful
  of `REACPW_*` environment variables and nothing else. There was no `REAC_RATE`
  reader in the daemon.
- **`~/.config/reac-pw/<iface>.env` was read by nothing.** The per-segment layer
  did not exist; the directory did not exist.
- **`~/.config/openmixer/reac.env` was reachable only through
  `reac-pw-master.service`'s `EnvironmentFile=`** — a unit that is installed and
  **disabled**. So the last-resort layer, whose entire purpose is the STANDALONE
  case, could not be reached standalone. A standalone `reac-pw --live ...` got
  nothing from it.

So the layering was real as an intention and absent as an implementation. It is
now in `src/reac_conf.{h,c}`, consulted by `main.c`, and pinned by
`tests/test_reac_conf.c` — which is sabotage-verified: swapping two layers and
accepting an empty value each turn the test red, and restoring turns it green.

## What to do, smallest first

- **Now, no code:** nothing urgent. The former "urgent" item was my misreading and
  is withdrawn. `REAC_RATE=96000` is correct and now agrees with the code default.
- **BUT NOTE, while this rig still runs 48 kHz:** every master invocation must
  keep `--rate 48000` EXPLICIT. The default is now 96000, so an invocation that
  omits `--rate` will bring up a 96 kHz master on a 48 kHz segment. That is a
  few-second re-handshake rather than a broken rig — see
  `docs/96K-SWITCH-ASSESSMENT.md` — but it is a dropout, and the startup
  provenance line is what makes it visible immediately.
- **SUPERSEDED — done, not "Next":** this bullet used to propose a templated
  `reac-pw@<iface>.service`, one unit instance per segment. That shape was
  brought to the operator and REJECTED
  (docs/design/specs/2026-08-20-reac-auto-spine.md §5, the openmixer tree):
  "a SINGLE daemon manages every box... Not a daemon per NIC." What actually
  landed is `packaging/reac-pw.service` — ONE unit, no `--live`/`--rate`/
  `--headamp` at all — opening one internal LISTENER per segment it HEARS on
  a linked Ethernet interface (the trunk-VLAN spec there, amendment
  2026-09-02; `REAC_IFACES` and the per-interface files retired with it), each
  then reading its OWN REAC_TX/REAC_ROLE/REAC_MIXER/REAC_NAME/REAC_HEADAMP/
  REAC_RATE from layer 3, the `<KEY>_<segment>` keys of the one
  `reac-pw.env` — consulted by N listeners in one process. `packaging/reac-pw.conf` is the commented worked example
  for this rig's own two segments; `docs/RIG-MASTERS.txt` carries the cutover
  note. **`~/.config/openmixer/reac.env` stays where it is and keeps what it
  has**; it is the floor, not a stray.
- **Done:** reac-pw prints the rate with the LAYER that produced it, so a
  disagreement between sources appears in the journal instead of on the wire.

## Clock configuration, same shape

`REACPW_CLOCK_FOLLOW` and `REACPW_CLOCK_REF` are reac-pw's, per segment, in the
same file. Nothing about the clock should ever reach the daemon through the
console: openmixer is a client of the segment's clock, not a source of it.

**Should free-running still be the default? NO, and it no longer is (0.5.0).** See
`docs/rig-data/2026-08-23-clock/README.md`. The short version measured on this
rig: following a reference would have changed nothing about the fault that was
actually dropping audio, because the reference was never wrong — the RME reads
−5.6 ppm and the boxes −6.1 and −20.0, while the pacer was −750. A daemon that
owns the clock and free-runs is misconfigured in principle, and it is now
disciplined by default — `ENV-KNOBS.md`'s procedure was walked on the rig, which
has run `REACPW_CLOCK_FOLLOW=1` + `REACPW_CLOCK_REF=Babyface` since 2026-09-07
20:55 without incident. `REACPW_CLOCK_FOLLOW=0` opts out and the free-run is
announced rather than silent. The caveat above stands and is the reason this
paragraph is kept: the flip is NOT a fix for the discards, because it is not one
— the pacer's own −750 ppm was, and `REACPW_CATCHUP_MAX_SLOTS` is what addressed
it.
