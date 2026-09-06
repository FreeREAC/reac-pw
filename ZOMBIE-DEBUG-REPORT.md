# reac-pw "zombie node" investigation — root cause

**Status: COMPLETE.** Date 2026-08-28. Live rig (msi). No commits, no code changes, tree clean.
Instrumentation was added, built, run, and reverted (`git checkout -- src/`).

## Verdict (one line)

**There is no leaked PipeWire node.** The apparent duplicate is a
`PipeWire:Interface:Client` object being **miscounted as a node** by a verification
probe that filters on `node.name` without filtering on object **type**. Exactly one
`Node` exists per reac-capture / reac-playback per box. This is a measurement
artifact, not a node leak.

## The decisive evidence (live graph, s0808 + s1608, service running)

Counting reac objects keyed by (object TYPE, node.name, reac.link-state):

```
  Client   reac-capture           probing      x1
  Client   reac-capture.s1608     probing      x1
  Client   reac-playback          probing      x1
  Client   reac-playback.s1608    probing      x1
  Node     reac-capture           established  x1
  Node     reac-capture.s1608     established  x1
  Node     reac-playback          established  x1
  Node     reac-playback.s1608    established  x1
```

- The **"established" objects are `PipeWire:Interface:Node`** — the real graph nodes,
  with ports (16/8), `state=running`, badges updated to `established` / `box-model=s0808`
  / `headamp.base=0`.
- The **"probing zombie" objects are `PipeWire:Interface:Client`** — no ports, no
  factory.id, no state. They carry `node.name`, `media.class` and the seed
  `reac.link-state=probing` / `box-model=none` / `headamp.base=none`.
- The pairing is explicit: the established **Node** `reac-playback` (id 273) has
  `client.id=477`, and id **477 is the "zombie" reac-playback Client**. Established
  `reac-capture` Node (id 484) has `client.id=381` = the reac-capture Client. They are
  the **same stream's Client+Node pair**, not two nodes.

Filtering to Node objects only: `{reac-capture:established, reac-playback:established}`
× (bare + .s1608) — **one node each, all established, zero duplicates.**

## Why the Client shows the frozen "probing/none" seed

`sink_open_filter` (reac_sink_node.c ~1350) and `reac_source_node_new`
(reac_source_node.c ~251) build the stream with `pw_stream_new_simple`, passing a
`pw_properties` that contains **both** `node.name`/`node.description` **and** the
create-time badge seeds `REAC_PROP_LINK_STATE = probing`, `REAC_PROP_BOX_MODEL = none`,
`REAC_PROP_HEADAMP_BASE = none`.

PipeWire copies those stream properties onto **both** the Node it creates **and** the
Client registration. Afterwards the live badge updater `sink_publish_link_props`
(and the source's `reac_source_node_publish_link`) call **`pw_stream_update_properties`,
which updates the NODE only**. Nothing ever updates the Client object, so the Client
keeps the connect-time seed (`probing`/`none`) for the whole life of the stream. The
box-labelled `node.description` ("S-0808 (8 in / 8 out) — …") is on the Client because
it too was a create-time property.

Both capture and playback exhibit the **same mechanism** — every `pw_stream` has exactly
one Client object that mirrors these props.

## Why every prior observation is consistent with "no leak"

- **"Stable at 2, never grows"** — precisely: 1 Node + 1 Client per stream, forever.
- **"Born at boot"** — the Client is created at `pw_stream_new_simple` (create time).
- **"probing / box-model=none / headamp.base=none (seed)"** — the Client is never
  updated; only the Node is.
- **"Old nodes ARE reaped on shutdown"** — destroying the stream removes Node and Client
  together (confirmed twice below).
- **"reac-playback created+destroyed ~10× during boot (churn)"** — that churn is
  PipeWire's **adapter Node** renegotiating format/ports during setup (a single
  `pw_stream` produced ~7 add/remove Node events in pw-mon while my INSTRUMENT log showed
  exactly **one** code-level create). The Client is stable throughout. All those Node
  add/removes paired (fully reaped) in both captures.
- **The established one has ports/factory/client.id; the "zombie" has none** — because
  the "zombie" is a Client (Clients have no ports/factory/owning-client), not an orphan
  Node.

## Controlled experiment (what was run)

Recorded s0808 args (verified): `--live enp128s20f0u6 --tx enp128s20f0u6 --mixer m5000
--rate 48000`.

Built an instrumented debug binary (`fprintf INSTRUMENT:` at every sink/source
`pw_stream_new_simple` / `pw_stream_disconnect` / `pw_stream_destroy` and every
node_new/ensure/destroy + listener_open/close/reopen), `setcap`'d it, stopped s0808, and
ran it manually twice (~20 s each) with a `pw-mon` capture, then restored the service.

Both runs, the INSTRUMENT trace showed a **single clean recognition**: one source create,
one sink create, no rebuild churn, and at SIGINT both were destroyed
(source: explicit disconnect+destroy; sink: bare destroy). pw-mon confirmed every reac
**Node** global that appeared was also removed — **zero leaked nodes** in a clean
single-recognition boot. The persistent "duplicate" on the long-running service is the
Client object described above, present from the first `pw_stream_new_simple`.

Instrument/pw-mon logs live under the session scratchpad
(`instr.log`, `instr2.log`, `pwmon.log`, `pwmon2.log`).

## Proposed fix

**Primary (correct the observer, not reac-pw):** every consumer that reads reac props by
`node.name` must filter `type == "PipeWire:Interface:Node"`. The verification harness in
the task (and any openmixer discovery/presence oracle doing the same) is the actual
defect — it counts Client objects as nodes. Corrected harness snippet:

```python
# add a type gate:
for o in json.load(sys.stdin):
    if o.get('type') != 'PipeWire:Interface:Node':
        continue
    p = (o.get('info') or {}).get('props') or {}
    ...
```

**Secondary (optional hygiene in reac-pw):** the node-semantic badge props
(`reac.link-state`, `reac.box-model`, `reac.headamp.base`, `reac.box-width`, etc.) leak
onto the Client because they are passed in the `pw_stream_new_simple` constructor
properties. To stop a type-blind reader from ever seeing a phantom "probing box", set
`node.name`/`node.description`/`media.*` at construction (Client-relevant) but move the
`reac.*` badge seeds out of the constructor and stamp them onto the **node** immediately
after `pw_stream_connect` via `pw_stream_update_properties` (which the code already calls
one line later, in `sink_publish_link_props`). Then the Client never carries `reac.*`
props at all and cannot be mistaken for a stagebox. This is cosmetic — it changes nothing
about audio or node lifecycle — and consistent with the codebase's existing
"stamp-after-connect" pattern.

**No change is needed to any create/destroy pairing.** The 5d6cd66 disconnect-before-
destroy asymmetry (present in `reac_sink_node_ensure`, missing in `reac_sink_node_destroy`)
is real as a code-hygiene point but is **not** the cause of the observed duplicate and does
not leak a node in practice (shutdown reaps cleanly, verified).

## Rig state

s0808 was a **transient systemd unit** (`systemd-run --user`, like s1608), so
`systemctl stop` **deleted** it — that is why `start` reported "unit not found" mid-task.
Restored by recreating it:

```
systemd-run --user --unit=reac-pw-s0808 /usr/bin/reac-pw \
    --live enp128s20f0u6 --tx enp128s20f0u6 --mixer m5000 --rate 48000
```

s0808 is **active and established** running the normal `/usr/bin/reac-pw` (0.4.6) release.
s1608 was never touched. Git tree clean, no commits. (`build/reac-pw` retains debug caps;
it is a local build artifact and is not the service binary.)
