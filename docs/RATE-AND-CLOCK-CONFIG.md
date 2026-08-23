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

**CORRECTION, and it is mine to own.** This file first read `REAC_RATE=96000` as a
stale value and a loaded gun on a 48 kHz rig. That was wrong, and it was wrong in
the most ordinary way: I found two numbers that disagreed and assumed the one
matching the running system was the intended one. **Operator ruling, 2026-08-23:
"96k is 96kHz and should be the default reac clock rate."** So the config file is
RIGHT and the running masters are what disagrees with the intent. The hazard
claim is withdrawn — `96000` in that file is now the same number as the code's
default, and `reac-pw --rate`'s default has been changed to match it.

A disagreement between a config and a running system does not tell you which one
is wrong. Only the person who chose the rate does.

What survives from the original reading is the SHAPE of the problem, and it
survives intact: the unit is disabled because it cannot express the rig — there
are TWO segments and it can start one — and a value nothing reads is a value
nothing corrects, whichever way it happens to be pointing.

There is a second, quieter problem. `reac.env` lives under
`~/.config/openmixer/`. The console's configuration directory holds the REAC
segment's sample rate, which is a property of the REAC rig and not of the desk
drawn on top of it. That is the console owning a fact it does not own.

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
   interface, named by the interface. Two segments means two files and two units,
   and any shape that cannot say that is the shape that ends up hand-started in
   tmux — which is exactly where the rig is now.

4. **It does not live under `~/.config/openmixer/`.** `~/.config/reac-pw/<iface>.env`
   (or `/etc/reac-pw/<iface>.env` for a fixed install). The console reads the
   segment's rate FROM the daemon — it is already published on the node — and
   never declares it.

5. **Auto-detect is a SLAVE's default and is wrong for a master.** A slave joins a
   segment somebody else is already driving, so detecting the rate is the only
   thing it can do. A master DEFINES the rate: on a silent segment there is
   nothing to detect, and "auto" resolved to whatever the code's fallback happened
   to be, with nothing on screen saying which. **Fixed:** a master with no `--rate`
   now takes `REAC_MASTER_DEFAULT_RATE` = **96000**, and reac-pw prints the rate
   WITH ITS PROVENANCE at startup — the command line, or the master default — so
   a 96 k master pointed at a 48 k segment says so in its first two lines instead
   of on the wire.

6. **THE DOORWAY IS `~/.config/reac-pw/<iface>.env`, AND `~/.config/openmixer/reac.env`
   SHOULD BE DELETED.** This is the one-store-one-writer question and it has a
   plain answer. Two files declaring one fact is the defect, regardless of whether
   they currently agree — and today they do not, which is only how it became
   visible. `reac.env` sits in the CONSOLE's configuration directory and declares
   a property of the REAC segment, which is a fact the console does not own; and
   it is read by exactly one unit, which is disabled. Move `REAC_RATE`,
   `REAC_LIVE_IFACE`, `REAC_TX_IFACE`, `REAC_MIXER`, `REAC_ROLE` and the clock
   knobs into the per-interface file, repoint the unit's `EnvironmentFile`, and
   **delete `~/.config/openmixer/reac.env` — do not leave it as a copy.** A second
   ledger that agrees today is a second ledger that will disagree later, and
   neither door announces the other. The console reads the segment's rate FROM
   the daemon; it is already published on the node.

## What to do, smallest first

- **Now, no code:** nothing urgent. The former "urgent" item was my misreading and
  is withdrawn. `REAC_RATE=96000` is correct and now agrees with the code default.
- **BUT NOTE, while this rig still runs 48 kHz:** every master invocation must
  keep `--rate 48000` EXPLICIT. The default is now 96000, so an invocation that
  omits `--rate` will bring up a 96 kHz master on a 48 kHz segment. That is a
  few-second re-handshake rather than a broken rig — see
  `docs/96K-SWITCH-ASSESSMENT.md` — but it is a dropout, and the startup
  provenance line is what makes it visible immediately.
- **Next:** move the two masters' invocations into `~/.config/reac-pw/<iface>.env`
  and a templated `reac-pw@<iface>.service`, replacing the single-segment unit and
  the tmux scope. The tmux scope is not a workaround anyone chose; it is what is
  left when the unit cannot describe the rig.
- **Then:** make reac-pw print the rate WITH ITS PROVENANCE at startup — given on
  the command line, from the environment, or auto-detected — so a disagreement
  between three sources appears in the journal instead of on the wire. A
  mechanical gate beats a rule anyone has to remember, and this file is currently
  a rule anyone has to remember.

## Clock configuration, same shape

`REACPW_CLOCK_FOLLOW` and `REACPW_CLOCK_REF` are reac-pw's, per segment, in the
same file. Nothing about the clock should ever reach the daemon through the
console: openmixer is a client of the segment's clock, not a source of it.

**Should free-running still be the default?** See
`docs/rig-data/2026-08-23-clock/README.md`. The short version measured on this
rig: following a reference would have changed nothing about the fault that was
actually dropping audio, because the reference was never wrong — the RME reads
−5.6 ppm and the boxes −6.1 and −20.0, while the pacer was −750. A daemon that
owns the clock and free-runs is misconfigured in principle, and it should be
disciplined by default once `ENV-KNOBS.md`'s rig procedure passes; but it must
not be flipped on as a *fix for the discards*, because it is not one.
