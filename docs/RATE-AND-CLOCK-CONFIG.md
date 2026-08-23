# Where the rate and the clock are configured, and why it is not openmixer

**omx is just the UI.** reac-pw owns the clock and the rate; openmixer is the
control surface and nothing more. Everything below follows from that one line,
and the current rig contradicts it in three places at once.

## The three sources that disagree today

| source | says | who reads it |
|---|---|---|
| the running masters (hand-started in a tmux scope) | `--rate 48000` | the kernel, twice |
| `~/.config/openmixer/reac.env` | `REAC_RATE=96000` | `reac-pw-master.service` — **installed but disabled**, so: nobody |
| `reac-pw --rate`'s own default | auto-detect on `--live` | reac-pw, when nothing else speaks |

**Enabling that unit today would put a 96 kHz master onto a 48 kHz rig.** It is
only harmless because it is disabled, and it is disabled because it cannot
express the rig: there are TWO segments and the unit can start one. The rate in
it is stale for exactly the reason a stale value always survives — nothing reads
it, so nothing corrects it.

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
   nothing to detect, and "auto" resolves to whatever the code's fallback happens
   to be. A master should be given its rate explicitly.

## What to do, smallest first

- **Now, no code:** delete `REAC_RATE=96000` from `~/.config/openmixer/reac.env`,
  or correct it to `48000`, so the disabled unit stops being a loaded gun. This
  is the only item that is urgent, because it is the only one that can put a
  96 kHz master on a 48 kHz rig by accident.
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
