# reac-pw review — 2026-09-24

A read-only audit of the reac-pw driver at `e7656a3` (1.0.26, after the #109 fix). No code
was changed. libreac is out of scope; where a finding depends on library behaviour it was
checked against a read-only clone of libreac `main` (`ee205b6`, 1.5.0, the floor
`meson.build` requires). The repo has no `CLAUDE.md` or `CONTRIBUTING`. The house rules this
review holds the code to come from `README.md`, `docs/*.md` and the design notes and specs
under `docs/design/`.

**Method.** Four passes covered the driver in full:
- the sink node, the qdisc and the formats;
- the recovery ladder, graph presence and the source node;
- `main.c` lifecycle, the helpers, and every exit path;
- P2 items: duplication, doc/log drift, and tests.

Every candidate kept below was then re-read against the code a second time, and the
line numbers are from that second read. A candidate that did not survive is listed under
[Discarded](#discarded-candidates) with the reason.

Verdicts:
- **CONFIRMED**: the failure path was traced end to end in the code.
- **PLAUSIBLE**: the code path is real, but the harm depends on runtime behaviour
  (PipeWire or kernel) that was not observed.

Severity counts, P1 and P2 together:

| severity | count |
|---|---|
| High | 3 |
| Medium | 14 |
| Low | 24 |

---

## Top 3

1. **H1: `fail_with_nodes` leaks a running slave engine, then the caller memsets it.** A
   refused open that already built a node leaves the slave thread running, the seglock
   held, and the socket and rings open. `hearing_serve` then zeroes the struct under the
   running thread. That is use-after-free, a leaked fd and thread, and a seglock this
   process can never claim again.
2. **H2: most roles never recover their nodes after a PipeWire restart.** Only the
   master-autodetect segments and the roster node check graph presence. Slave, box-role,
   box-master-join, tap, door and pcap nodes die with the server and stay dead with no log
   line. This is the 2026-09-23 incident again, for every role except one.
3. **H3: the netns suite, including the PipeWire-restart regression test, cannot fail in
   practice.** It has never run in CI (release job: `Ok: 0 Skipped: 30`). Seven of those
   tests also report a daemon that dies at startup as **SKIP**, not FAIL.

---

## P1 — High

### H1. A refused open after node creation leaks the engine, seglock, socket and rings, then memsets a live slave

**Where:**
- `src/main.c:2517-2523` is the `fail_with_nodes:` label.
- It is reached from `main.c:2428` (the `--box` pin: ensure failed) and from
  `main.c:2486` (the slave, box or box-master capture ensure failed).
- The callers are `main.c:3385` (`memset(L, 0, sizeof *L)` in `hearing_serve`) and
  `main.c:2581` / `2626` (rate and role re-open).

**What happens.** The label calls only `listener_drop_nodes(L, loop); return -1;`. Its own
comment says the ring, socket and seglock "are cleaned up at each refusal above". Nothing
above this label has cleaned them.

- **Slave, join or box path.** `reac_slave_open` and `reac_slave_start` have already
  succeeded (`main.c:2294-2296`), so the slave thread is transmitting from `&L->slave` and
  `&L->tx_ring`. Then `reac_source_node_ensure` fails at `main.c:2481`, for example
  because PipeWire is not up yet at boot, which `main.c:5019` says can happen.
  `hearing_serve` then memsets `L` under the running thread. The thread, its AF_PACKET
  socket, the RX socket and both rings leak. The retry 5 s later reuses the same slot.
- **Master pin path.** The seglock claimed at `main.c:2134` stays bound. The next
  `listener_open` calls `reac_seglock_init` (`main.c:1878`), which forgets the handle.
  From then on this process refuses the segment with "another process already holds
  that segment", and the holder is itself.

**Severity:** High. It is memory corruption on the slave path, and a segment this process
cannot re-take on the master path.

**Smallest fix.** At `fail_with_nodes`, call `listener_close(L, loop)` instead of
`listener_drop_nodes`. `listener_close` is documented as safe on a partially opened
listener, and it already stops the slave and releases the rings, the RX and the seglock
in the right order.

### H2. No graph-presence recovery for any node outside master autodetect

**Where:**
- `src/main.c:2393` starts the ladder's timer only for `c->role == REAC_ROLE_MASTER && L->sink`.
- `reac_source_node_on_graph` and `reac_sink_node_on_graph` are called only at
  `main.c:834` and `main.c:1006`, both on that timer.
- The roster node has its own check (`main.c:5000`).

**Scenario.** A recorder (slave), a `role = box` segment, a box-master join, a tap, a
refusal door or a pcap/no-TX master capture is running, and `pipewire.service` restarts.
Each `pw_stream` goes UNCONNECTED and nothing ever asks. The segment has no nodes until
the daemon itself restarts. Meanwhile the roster node, which does rebuild, keeps
reporting the segment as `slave` or `tap`, so the console sees a row with no device.

This is the #109 sibling "a node that failed is left dead", and the 2026-09-23 desk
incident (`tests/graph-survives-a-pipewire-restart.sh`) for every role except the one that
test covers: it runs only `--live … --tx`, i.e. master.

**Severity:** High.

**Smallest fix.** `on_rate_reopen_timer` already visits every opened listener every 200 ms.
Give each non-autodetect listener a `struct reac_node_recover` and run
`reac_node_recover_step_pair` there. On REBUILD:
1. destroy the dead side;
2. call `reac_source_node_ensure` / `reac_sink_node_ensure`;
3. re-wire what is set only at open time: `reac_source_node_set_role_swap`, then
   `listener_publish_segment` or `publish_box_master`, and per-stream cfg for taps.

Extend the restart test to a slave and a tap segment.

### H3. The integration suite cannot fail where it runs, and it does not run in CI

**Where:**
- `.github/workflows/release-rpm.yml:52-53` is the only workflow and is `workflow_dispatch`
  only. Tests run only in the spec's `%check`.
- The 1.0.26 release job (run 36049836895, job 107802347856) reports `Ok: 79 Skipped: 2`
  for unit tests and `Ok: 0 Fail: 0 Skipped: 30` for the netns suite.
- A daemon that dies at startup is reported as SKIP in `tests/no-box-no-node.sh:96` and
  `:128`. The inner body prints `daemon-died` and exits 91. The outer script turns any
  non-zero rc into `exit $SKIP` at line 128, before it reaches
  `grep '^daemon-died' && fail` at line 135.

**The same inner-exit → skip-line → unreachable-FAIL shape** appears in:

| test | inner exit | skip line | unreachable FAIL |
|---|---|---|---|
| `graph-survives-a-pipewire-restart.sh` | 97, 123 | 149 | 156 |
| `empty-master-yields-the-budget.sh` | 119 | 215 | 217 |
| `graph-state-follows-segment-state.sh` | 137 / 179 | 226 / 247 | 228 / 249 |
| `roster-keys-leave-with-their-segment.sh` | 118 | 147 | 149 |
| `roster-node-lists-every-segment.sh` | 156 | 189 | 206 |
| `declared-vlan-waits-for-its-tag.sh` | 120 | 163 | 174 |

**Scenario.** A startup regression, such as a crash in `listener_open`, passes every
automated check. On a desk with namespaces it reads as SKIP.

**Severity:** High. Not a runtime defect, but it removes the proof behind H2, M1 and the
#934/#109 fixes.

**Smallest fix.**
- Skip only on rc 77: `[ $rc -eq 77 ] && exit 77`.
- Check the `daemon-died` marker before any rc-based skip.
- Treat every other non-zero rc as FAIL.
- Add a push/PR workflow on a runner that allows user namespaces and has PipeWire, or at
  least fail `%check` when the whole netns suite skipped.

---

## P1 — Medium

### M1. GIVE_UP is terminal while the box stays enrolled, contrary to the header and the log line — CONFIRMED

**Where:**
- `src/reac_node_recover.c:46`: `if (r->gave_up) return REAC_RECOVER_WAIT;`
- `main.c:1037-1045` is the give-up line.
- `reac_node_recover.h:26` and `:37` are the header claims.

**Scenario.** PipeWire is down longer than the ladder. The windows are
10+20+40+80+160+160 ticks × 200 ms = 94 s, for example after a start-limit hit or a user
session restart. Every rebuild lands on a dead server, and the ladder gives up.

When the server returns, a stream that is ERROR or UNCONNECTED never reconnects on its own.
`ensure` is only called again on a model change, so the node cannot "appear". The
enrolled box's segment has no nodes for the life of the process. Meanwhile the roster,
which retries forever, says `established`.

The line claims "it is retried the moment the node appears or the box is re-recognized".
The header claims "never a permanent verdict" and "~1 minute … ride out a session manager
restart". The ladder actually gives up after 94 s, and the verdict is permanent.

**Severity:** Medium.

**Smallest fix.** After GIVE_UP, keep rebuilding silently at the ceiling window (one
attempt every 32 s, no line). Or reset the ladder when the roster node's own rebuild
succeeds, which is already the "server is back" signal.

### M2. A pinned `--box` segment is never checked while its box is absent — CONFIRMED

**Where:** `src/main.c:986` is `if (c->last && !c->pinned)`, followed by
`return; /* nothing recognized yet */` at `:998`, before the ladder at `:1000`.

**Scenario.** A fixed install is pinned `--box s1608`, and the nodes are built at boot
(`main.c:2413-2435`) precisely so the patch exists before the box is powered. PipeWire
restarts while the box is off. Both pinned streams go UNCONNECTED and the early return
skips the ladder. The pin's patch is dead until the box is powered, which defeats the
pin's stated purpose.

**Severity:** Medium.

**Smallest fix.** When `bm == NULL && c->pinned`, run the same `step_pair` block before
returning. On REBUILD, the ensure calls use `c->pin_model` / `c->pin_label`.

### M3. A pinned segment's ladder is not reset when the box leaves and returns — CONFIRMED

**Where:** `src/main.c:986`. For a pinned segment, `c->last` survives the departure, so the
same model coming back takes the `bm == c->last` arm (`:1000`). That arm never calls
`reac_node_recover_init`.

**Scenario.** The ladder gave up while the box was present. The operator power-cycles the
box, as the give-up line suggests ("the box is re-recognized"), and the ladder stays in
`gave_up` WAIT for ever.

**Severity:** Medium. It is a sibling of M1.

**Smallest fix.** In the pinned `bm == NULL` arm, call `reac_node_recover_init(&c->recover)`,
or reset it on the NULL → non-NULL edge.

### M4. A rebuilt reac-playback never republishes `reac.rate*` or `reac.role*` — CONFIRMED

**Where:**
- `src/reac_sink_node.c:1876-1894`: `sink_open_filter` resets the link, head-amp, disco and
  arbitration shadows, but not `rate_hz_last`, `rate_asserted_last`,
  `rate_reestablishing_last`, `rate_refused_last`, `role_state_last` or
  `role_refused_last`.
- The creation props (`:1811-1864`) do not carry those keys, and the compare at
  `:1105-1107` / `:1201-1202` then returns early.

**Scenario.** Any rebuild with the rate and role unchanged loses those keys until one of
them next changes:
- a box leaves and returns (unpublish `main.c:992` → ensure `main.c:1095`);
- a box swap;
- a ladder rebuild after a PipeWire restart.

The console's rate and role controls then have no readback: `reac.rate`,
`reac.rate.source`, `reac.cfg.rate.state`, `reac.role` and `reac.cfg.role.state` are all
missing.

**Severity:** Medium. The trigger is common: every re-enrolment.

**Smallest fix.** In `sink_open_filter`, next to the other shadow resets, add
`n->rate_hz_last = -1; n->role_state_last = NULL; n->role_refused_last = -1;`, and reset
the other rate shadows too.

### M5. `--set KEY=VALUE` is announced as applied for about 17 knobs it never reaches — CONFIRMED

**Where:**
- `src/reac_knobs.c:96-112` stores `--set` values in a private `g_argv`. Only
  `reac_knobs_resolve*` reads them.
- These reads bypass `g_argv`:
  - `reac_conf_flag` (`reac_knobs.c:69-76`);
  - `reac_conf_lookup` at `main.c:1288` (REAC_TX), 1351 (MIXER), 1359 (NAME),
    1368 (HEADAMP), 1383 (SRC_MAC), 1405 (BOX_CHANNELS), 1437 (REAC_RATE),
    2033 (CLOCK_FOLLOW), 2036 (CATCHUP_MAX_SLOTS), 2038 (RATE_MATCH) and
    2269-2278 (the BOX_MASTER_* knobs);
  - the pacer and RT knobs libreac reads itself: REACPW_PACER, PACER_LEAD_US and RT_PRIO.

**Scenario.** `reac-pw --set REACPW_PACER=thread` prints
`S_KNOB_SET knob REACPW_PACER=thread (cli)` and runs ETF anyway. Likewise
`--set REACPW_RATE_MATCH=1` is announced and rate matching stays off. The daemon reports
success for a setting that did nothing.

`tests/set-flag-reaches-the-daemon.sh` checks only the announce line, using REAC_DEBUG,
one of the few keys that do go through `reac_knobs_resolve`. It therefore cannot see this.

**Severity:** Medium.

**Smallest fix.** Have `reac_knobs_set_argv` also `setenv(key, value, 1)`. The environment
is the top layer every `reac_conf_*` reader, libreac included, consults. Add one test that
checks an effect, for example the `reac-pacer: backend` line under
`--set REACPW_PACER=thread`.

### M6. A vacant tap door leaks its RX socket and ring on every teardown — CONFIRMED

**Where:**
- `src/main.c:1885-1899`: `LISTENER_TAP_VACANT` falls through to the door.
- `main.c:1931`: `reac_rx_open` opens the AF_PACKET socket and allocates the ring.
- `main.c:1979-1984`: the vacant door returns 0 without starting RX.
- `listener_close`'s tap branch (`main.c:2533-2540`) calls `listener_close_tap`
  (`main.c:1654-1664`) and `listener_drop_nodes`, and neither closes `L->rx` or frees
  `L->ring`. `hearing_serve`'s memset then loses the pointer.

**Scenario.** A pinned tap on an intermittent mirror port leaks one socket fd and one ring
of about `40 × pow2(rate/4)` floats each time the door is replaced, dropped or re-opened.

**Severity:** Medium.

**Smallest fix.** Skip `reac_rx_open` when `door_vacant`, since nothing reads it. Or add
`reac_rx_close(&L->rx); reac_ring_free(&L->ring);` to the tap branch of
`listener_close`; both are NULL-safe.

### M7. The wake ladder can leave the NIC admin-down — CONFIRMED

**Where:**
- `src/main.c:930-940` takes the link down and sets `c->up_at_ns`.
- Only a later turn of the same timer brings it back up (`main.c:863-876`).
- `listener_drop_nodes` (`main.c:1830-1835`) destroys that timer without checking
  `up_at_ns`.

**Scenario.** SIGTERM arrives, or a rate or role re-open or a hearing drop closes the
listener, inside the 1.2 s window (`REAC_WAKE_DOWN_MS`, `reac_wake.h:58`). The device is
left `down`.

A restart does not repair it:
- hearing mode will not sniff a link that is down;
- a re-opened master sends nothing, so its new ladder holds on `REAC_WAKE_NOTHING_SENT`
  and never raises the link.

An operator restarting the daemon over a silent box is exactly when this window is open.

**Severity:** Medium.

**Smallest fix.** In `listener_drop_nodes`, before destroying `ad_timer`:
`if (L->adc.up_at_ns) { reac_link_admin(L->adc.ifname, 1); L->adc.up_at_ns = 0; }`.

### M8. With several `--live` segments, the link-budget admission and the wake sibling guard are off — CONFIRMED

**Where:**
- `link_used_kbit` (`src/main.c:2798`), `link_budget_holders` and `port_siblings_served`
  (`main.c:2892`) walk `g_hear.listeners` / `g_hear.n_slots`.
- Those fields are set only in `hearing_start` (`main.c:4784-4785`), which runs only in
  hearing mode (`main.c:5739`).

**Scenario.** `--live enp131s0,enp131s0.11,enp131s0.12,enp131s0.13` gives every listener
`used = 0`:
- all four masters are admitted onto one 100 Mbit port, which is the 387 Mbit/s overload
  the comment at `main.c:2077-2091` exists to refuse;
- `siblings_served` is always 0, so `wake_step` will bounce a parent that is carrying live
  VLAN siblings.

**Severity:** Medium.

**Smallest fix.** In the `--live` path, set `g_hear.listeners = listeners;
g_hear.n_slots = n_listeners;` without enabling hearing, or pass the table to the three
walkers.

### M9. A `role = box` segment never drains its capture node's role door and never republishes its answer — CONFIRMED

**Where:** `src/main.c:5177`: `if (!L->sink || L->cfg.join_box_master) { … take_reopen_role(L->src) … listener_publish_segment(L); continue; }`.

**Scenario.** A box-role segment builds an upstream-carrier sink (`main.c:2327-2345`), so
`L->sink` is set and `join_box_master` is 0. The poll takes the sink branch instead:
- the capture node, which received the role swap at `main.c:2501`, latches a
  `reac.cfg.role` write in `reopen_role` that nobody takes, and its answer stays
  `role_reestablish_pending`;
- `listener_publish_segment` ran once at open and never again, so `heard`, the master MAC
  and `established` are frozen.

The branch comment predates the box role (spec 2026-09-17).

**Severity:** Medium.

**Smallest fix.** Make the condition `(!L->sink || L->cfg.join_box_master || L->cfg.box_model)`.

### M10. The slave-side reac-playback publishes itself as the master's door — CONFIRMED (code); console effect PLAUSIBLE

**Where:**
- `src/reac_sink_node.c:1999` seeds `role_state = APPLIED` before the `upstream_ring` early
  return at `:2048-2054`.
- `sink_open_filter` always calls `sink_publish_role_props` (`:1210`, `reac.role` =
  `REAC_CFG_ROLE_VALUE_MASTER`), `sink_publish_disco_props`, and stamps `reac.segment`
  (`:1823`).
- For the upstream carrier, `main.c:2356` calls ensure before `set_role_swap`
  (`main.c:2392`), and that node has no log timer to correct it later.
- The comments at `reac_sink_node.c:1175-1180` ("reac_sink_node_new is never called for a
  slave") are false since 0.5.6.

**Scenario.** On a box-master join or a `role = box` segment, the segment's two nodes
disagree: reac-playback says `reac.role=0` (master) and `applied`, while reac-capture says
slave. Both carry the same `reac.segment`, the key a console reads its row from.

**Severity:** Medium.

**Smallest fix.** When `n->upstream_ring` is set, skip the role, disco and rate publishers
and the rate/role write door in `sink_open_filter` and `on_param_changed`, and correct the
comments.

### M11. With drop-ins and no hand-written file, the segment conf is re-read on every lookup — CONFIRMED

**Where:**
- `src/reac_segconf.c:441` stores the hand-written file's stamp in `stamp[0]`, but
  `read_one` only advances `n_files` when the file is there (`:329-330`).
- `load_dropins` then uses `slot = c->n_files` (`:403`), which is 0, so the first drop-in
  overwrites `stamp[0]` with `present = 1`.
- In `reac_segconf_refresh` (`:472`), `moved(REAC_SEGCONF_FILE, &stamp[0])` compares an
  absent file with a present stamp and always answers "moved".

**Scenario.** This is exactly the layout openmixer writes: drop-ins only. Every
`reac_segconf_refresh` re-reads and re-parses the whole directory on the main loop. It is
called from `main.c:1305`, `2933`, `3044` and `3071`, including per unserved sniffer on
every roster tick.

**Severity:** Medium. It is wasted I/O on every tick, and any once-per-load notice
repeats.

**Smallest fix.** Give the hand-written file its own stamp field (`stamp_main`), or start
drop-in slots at 1.

---

## P1 — Low

| id | where | scenario → wrong outcome | fix shape | verdict |
|---|---|---|---|---|
| L1 | `reac_sink_node.c:2275`, `:2292` (ensure, unpublish) vs `:1035-1036` | With `REACPW_RATE_MATCH=1` (opt-in), a rebuilt stream on a same-rate link gets no new `SPA_IO_RateMatch`, and `on_process` (`:415`) writes `n->rate_match->rate` into the destroyed stream's IO area. `rate_match_present` is never cleared, so `reac.health.rate-match-ppm` shows a stale value instead of `n/a`. | Null `rate_match` and zero `rate_match_present` wherever `position` is nulled | PLAUSIBLE (depends on whether PipeWire emits RateMatch=NULL on teardown) |
| L2 | `reac_qdisc.c:30-31`, `:51-58` | Our ETF install was verified, the pacer refused ETF, and `reac_qdisc_disarm` runs. A transient netlink failure makes the table UNREADABLE. The record was already memset, so the function returns 0 and `release` skips the qdisc at exit, which contradicts `reac_qdisc.h`'s "our record survives". | On UNREADABLE with `ours.installed`, restore `*q = ours` and return -EIO | CONFIRMED |
| L3 | `reac_qdisc.c:108-112` | The kernel ACKs the ETF add, the read-back fails, and the function returns -EIO with `installed = 0`. If the qdisc exists, nobody removes it at exit. | Keep `ifindex`/`installed` after an ACKed add; `release` already re-verifies | CONFIRMED |
| L4 | `main.c:834` | The "autodetected … reac-capture N in / reac-playback M out" line is gated only on the capture being on the graph. When the sink ensure failed (`:1095`), it announces a playback that is not there, and ~2 s later the ladder says so. | Also require `reac_sink_node_on_graph(c->sink, NULL)` | CONFIRMED |
| L5 | `reac_node_recover.c:179` | One attempt budget and one doubling window serve both sides. If the capture fails 3 times and the playback is gone on the tick the capture recovers, the playback's first rebuild waits 16 s instead of 2 s, and it gets only 2 attempts. | Reset `attempts` when the set of gone sides changes | CONFIRMED |
| L6 | `main.c:1047`, read at `:1033` | `c->rebuilt` keeps only the last attempt's subject. If attempt 1 took both nodes and attempt 2 took only the capture, the line says "reac-capture is back". | OR the sides across attempts; clear them on reset | CONFIRMED |
| L7 | `main.c:2407-2412` | `pw_loop_add_timer` fails and nothing is said. The master segment never builds nodes and never runs either ladder, but still prints "MASTER autodetect — PROBING". | Print a code and fail the open | CONFIRMED |
| L8 | `main.c:5024-5031` | `static int said` means `RC_E_ROSTER_NODE` is printed once per process. A second PipeWire outage says only "rebuilding it" and then nothing while creation keeps failing. | Reset `said` after a successful build | CONFIRMED |
| L9 | `main.c:2364-2371` | The slave engine fails to open or start (no CAP_NET_RAW). The open still returns 0 and `hearing_serve` logs "segment up (slave…)" with no TX. The master path refuses the same case on purpose (`:2159-2168`). | Treat it as a refusal: free `tx_ring`, rx and ring, return -1 | CONFIRMED |
| L10 | `main.c:2581-2583`, `2586` | A failed rate re-open on a heard segment leaves `opened = 0` while ifscan still marks the interface served, so nothing retries it until a link edge or a restart. | Call `reac_ifscan_serve_failed(…)` on both failure arms | PLAUSIBLE (ifscan state machine not traced end to end) |
| L11 | `main.c:2402`, `930` | On a VLAN segment `ifname` is `<parent>.<vid>`. Toggling the VLAN netdev makes no PHY edge, yet the lines at `:872-875` and `:923-928` say the box "sees this as PHY LINK-UP". | Refuse the ladder on VLAN devices, or bounce the parent under the sibling guard | CONFIRMED |
| L12 | `main.c:5818-5823` | `hearing_stop` deletes the minted VLAN netdevs before the listeners that transmit on them are closed. A clean stop can log send errors or "iface lost". | Close the listeners first, then call `hearing_stop` | PLAUSIBLE |
| L13 | `main.c:573` (`parse_headamp`) | `%u` is not range-checked, so `0:phantom:4294967297` parses as value 1 and turns 48 V on. `REAC_SRC_MAC` `%x` has no width (`:1385`). `atoi` is used for REAC_RATE, BOX_CHANNELS and similar. `(int)(f+0.5f)` on NaN is undefined behaviour in `reac_role_cfg.c:47`, `reac_rate_cfg.c:117` and `reac_headamp_prop.c:36`. | `strtoul` with end, errno and bounds checks; `%2hhx`; `isfinite()` | CONFIRMED |
| L14 | `main.c:187-188` (`split_list`) | Tokens are clamped to IFNAMSIZ (16) although the comment says 15. The later clamp to 15 can alias a different real interface. | Reject tokens with `len >= IFNAMSIZ` | CONFIRMED |
| L15 | `reac_sink_node.c:2380-2388` | `n->pacer.on_session = sink_on_session;` sits outside the `if (n)` braces (misleading indentation), so a NULL `n` crashes. Today only a non-NULL `n` is passed. | Move it inside the braces | CONFIRMED (latent) |
| L16 | `reac_source_node.c:141-151` | Two `snprintf` calls with `%s` run in the RT `on_process`, under `REAC_DEBUG` only and once per driver change. They are bounded, but break the file's own "RT does not format" rule. | Copy the raw name and format it in `drain_log` | CONFIRMED (debug-only) |

---

## P2 — Medium

### M12. Only the capture node has a stream `state_changed` handler — CONFIRMED

The source prints `stream ERROR — <reason> (this node is NOT in the graph…)` at once
(`src/reac_source_node.c:291-306`). The sink's `stream_events` (`src/reac_sink_node.c:858-863`)
has none. A reac-playback refused by PipeWire is silent until the ladder's grace window
runs out, and on the roles without a ladder (H2) it is silent for ever.

**Fix:** move `on_state_changed` into `reac_node_graph.c` next to `reac_node_on_graph`, and
register it on both nodes. This is the same "one law, one copy" move that `f3e4f03` made.

### M13. `tests/asserted-lines-have-a-producer.py` never sees about a third of the asserted patterns — CONFIRMED (regex)

Its `PATTERN_CALL` (lines 32-34) misses:
- single-quoted patterns;
- combined flags (`-qa`, `-qaE`, `-Eq`);
- `grep -q -- '…'`;
- `wait_for_since N "…"`.

Examples it never checks: `link-up-reestablishes.sh:84`, `etf-qdisc-owned.sh:268-278` and
`master-gone-is-re-decided.sh:96-105`. The spot-checked ones all have producers today, some
of them in libreac, so nothing is broken yet. The ratchet is blind, though.

**Fix:** widen the regex and scan the libreac sibling checkout when present, as
`reac-code-conformance.py` does.

### M14. `docs/NODE-PROPERTIES.md` and `docs/RATE-AND-CLOCK-CONFIG.md` contradict the code — CONFIRMED

`NODE-PROPERTIES.md`:
- `:35` lists `reac.cfg.rate` as writable on **capture**, but the source's
  `on_param_changed` (`reac_source_node.c:267-282`) handles only `reac.cfg.role`.
- `:88-90` lists box-source, firmware, hw, `reac.rate.source` and `reac.rate.drivable` on
  capture. `publish_link` and `publish_segment` stamp none of them.

`RATE-AND-CLOCK-CONFIG.md`:
- `:43` says a layer that answers nonsense is "named and skipped". `listener_resolve_rate`
  (`main.c:1438-1451`) names it and then drops straight to the built-in, never trying the
  lower layers.
- `:40` names `REAC_MASTER_DEFAULT_RATE`, which no longer exists; the code uses
  `reac_rate_best_drivable`.

**Fix:** correct the tables, or make the nonsense path continue to the next layer.

---

## P2 — Low

| id | where | finding | fix shape |
|---|---|---|---|
| L17 | `reac_sink_node.c:1027-1092`, `reac_source_node.c:581-624` | Duplicated live-rate reconnect paths are now unreachable: nothing in reac-pw calls `reac_pacer_request_rate`, and accepted rates go through `reopen_rate` → full re-open. The copies have already drifted: the source does not null `position`, the sink does. Comments at `sink:574` and `:756` still describe the old path. | Delete both paths and their sabotage tests |
| L18 | `reac_sink_node.c:354-359` vs `reac_source_node.c:122-128` | The graph-clock sample is taken before any early return in the source (by design; see its comment) but after the `!position` / `!pwb` returns in the sink. | Hoist the sink's publish to the top of `on_process` |
| L19 | `tests/test_reac_node_ensure.c:82-92`; `test_reac_sink_format.c:168-175`, `206-212`; `test_reac_source_format.c:118-126` | The "sabotage" checks assert constants such as `CHK((8==8)==1)`, which cannot fail. `test_reac_source_format` links only `reac_sink_format.c`, so it cannot catch source-node drift (L17). | Delete the constant checks; test the real call sites |
| L20 | `tests/test_reac_knob_table.c` vs `docs/ENV-KNOBS.md:145` | The doc says the test fails when a knob the code reads is missing from the doc. The test only compares the table with the doc and never scans `src/` for `reac_conf_lookup` keys, which is also why M5 went unseen. | Grep `src/*.c` for looked-up keys and require each in the table |
| L21 | `tests/reac-code-conformance.py:33` | `FLOOR = 30` while the script itself reports 29 bare refusal lines, so the ratchet has one line of slack. | Set `FLOOR = 29` |
| L22 | `docs/HEALTH-TELEMETRY.md` | Three published keys are missing from the table: `reac.health.slot-debt-max`, `reac.pace.backend` and `reac.pace.backend-refusal` (`reac_sink_node.c:1536-1546`). `rate-match-ppm = n/a` also means "REACPW_RATE_MATCH off", which is the default. | Add the rows and the sentence |
| L23 | `README.md:25`, `:40-41` | Says libreac / libreac-transport `>= 1.0.1`. `meson.build:96,119` and the spec require `>= 1.5.0`. | Fix the floor |
| L24 | Stale comments | `reac_source_node.h:6` describes a `pw_filter` with MAX ports, but it is a `pw_stream`. `reac_source_node.h:199` says "no reac.segment", but it is stamped. Several places in `reac_sink_node.c` / `.h` say `pw_filter` (`:1592`, `:2184`, `:2258`, header `:143-157`). `main.c:662-666` says `--box` is "RETIRED … ignored", but it pins. `explain_tx_failure` (`main.c:552`) omits `cap_net_admin`. `reac_qdisc.h`'s "never stripped by us" holds only for the sweep, because install replaces an operator's root qdisc and release deletes it. | Edit the comments and text |

---

## #109 siblings: the ladder class, collected

The #109 fix itself holds. `reac_node_recover_step_pair` destroys a side only on its own
`*_gone` flag, and quotes that side's own reason. The same class, "a node rebuilt that did
not fail / one left dead that did", recurs in:

| kind | finding |
|---|---|
| left dead | H2 (roles with no ladder), M1 (GIVE_UP is terminal), M2 (pinned segment with no box), M3 (pinned segment's ladder not reset) |
| ladder counts the wrong thing | L5 (shared budget across sides), L6 (subject of the "back" line) |
| rebuilt, but not whole | M4 (rate/role props lost on rebuild), L1 (stale `rate_match`) |
| success reported for an absent side | L4 (announce line) |

No case was found of a healthy node being rebuilt for its sibling's failure after c749d43.

## #934 class: lifetime across PipeWire restarts

No use-after-free was found across a restart.
- Every node is a `pw_stream_new_simple` stream that owns its own context and core, and
  `pw_stream_destroy` removes the stream's listener. There is no shared registry or
  `spa_hook` in main.c that could outlive a core.
- The roster node tears down filter → core → context.
- Timers are destroyed before the nodes they rebuild (`listener_drop_nodes`).
- Capture is destroyed before the sink whose pacer it borrows.
- `unpublish` keeps the sink struct alive.

The lifetime defects that were found are on the *open-failure* path (H1) and in an IO area
pointer (L1), not across a restart.

## RT safety

- **Sink `on_process`** (`reac_sink_node.c:333-536`, `PW_STREAM_FLAG_RT_PROCESS`): no
  allocation, logging or locks. Loops are bounded by `nframes` ≤ quantum and channels ≤ 40.
  The buffer is re-queued on every path after dequeue.
- **Source `on_process`**: the same, except the debug-only formatting in L16.
- **Pacer SCHED_FIFO thread**: its only callback into reac-pw is `sink_on_session`, which is
  allocation-free.
- **Everything else** that logs, does netlink or calls `pw_stream_update_*` runs on the
  main loop.

**Verdict:** clean apart from L16.

---

## Discarded candidates

| candidate | reason discarded |
|---|---|
| Upstream-carrier sink closes fd 0 on destroy (`reac_linkmon_close` on a calloc'd, never-opened linkmon) | In libreac ≥ 1.5.0, `struct reac_linkmon` holds a `struct reac_handle *`. calloc leaves it NULL and `reac_handle_close` is NULL-safe (`reac_handle.c:22-30`). It would only be real against a pre-handle libreac, which the meson floor excludes. |
| Ladder verdict sides are taken from the current tick while absence is counted across ticks, so a merely-connecting side could be destroyed | The only self-clearing not-on-graph state is "CONNECTING, no id" right after a build. ERROR, UNCONNECTED and a NULL slot are terminal, so the side named gone at the verdict tick has really failed. The residual cost is L5. |
| `sink_on_graph` when the sink is legitimately absent (a capture-only box) forces endless sink rebuilds | The ladder only exists when `L->sink` exists, and every libreac model row has `out_ch > 0`. A NULL sink stream only comes from a failed ensure or an unpublish, both of which are real absences. |
| Timer use-after-free when a listener is closed while its `ad_timer` event is pending in the same dispatch | SPA's `loop_remove_source` clears pending epoll entries, and no source is destroyed from inside its own callback. |
| `realloc` of arrays whose element pointers are held elsewhere | There are none. The only `malloc` is `reac_segconf.c:334`, and every table is fixed-size. |
| `reac_sink_node_new` error paths leak | They are clean. A pacer open or start failure closes the pacer, releases the qdisc and frees `n`. Linkmon and the timer are acquired later. |
| Race between `process()` and ensure, unpublish or rate reconnect | `pw_stream_disconnect` / `destroy` quiesce the data-loop callback synchronously. |
| Source rebuild uses the boot `cfg.sample_rate`, not the live rate | The rate never changes in place (see L17); an accepted rate re-opens the whole listener and re-copies `adc.scfg`. |
| Main loop reads the pacer's non-atomic `pacer.master.box_mac` / `session_seq` (torn read) | Not kept. A torn read here is self-correcting on the next 200 ms tick, and the RT-side `sink_on_session` reset is the authoritative path. It was noted as a hardening item, not a defect. |
| A rate reconnect that fails is never retried (`sink_publish_rate_props` commits its shadows first) | Unreachable, for the same reason as L17. |
| Unregistered or never-built tests | Every `tests/*.c`, `*.sh` and `*.inc` is referenced in `meson.build`. |
| `test_reac_node_recover` / `test_reac_watch` assertions are weak | The assertions are real and the loops are bounded. |
| ENV-KNOBS names, count and defaults drift from `g_reac_knobs` | They agree: 28 knobs, and the defaults match. |
| `link_budget_holders` snprintf overflow | Clamped, with an `n + 1 >= cap` break. |
| /proc parsers (`seglock_inode_of`, `describe_pid`, `read_cap_effective`) overflow | Widths are bounded and output is NUL-terminated. |
| `RATE-AND-CLOCK-CONFIG.md` precedence row 3 (segment key vs env) | Its own wording ("outranks the bare key in every layer below") is ambiguous rather than wrong, so it was not kept. The real mismatches are in M14. |

---

## Coverage

| area | files read | verdict |
|---|---|---|
| Recovery ladder | `reac_node_recover.c/.h`, `reac_node_graph.c/.h`, `reac_node_ensure.c/.h`, `main.c:780-1108`; `git show c749d43 f3e4f03` | #109 fix holds. Siblings: H2, M1-M3, L4-L6 |
| Playback node (RT and main loop) | `reac_sink_node.c` (all 2431 lines), `reac_sink_node.h`, `reac_sink_format.c/.h` | RT clean. M4, M10, M12, L1, L15, L17, L18 |
| Capture node | `reac_source_node.c/.h` | RT clean except L16. M12 (asymmetry), L17, L24 |
| Qdisc ownership | `reac_qdisc.c/.h`, `git show 9aee2b6`, `tests/test_reac_qdisc.c` | #109 nits 2 and 3 hold. L2, L3, L24 |
| Listener open/close, exits | `main.c` (all 5830 lines, in chunks) | H1, M6, M7, M9, L7, L9, L10, L12 |
| Hearing, ifscan, link budget, wake | `main.c:2790-2900`, `4779-4830`, `reac_wake.c/.h`, `reac_link_budget.c/.h` | M8, L11 |
| Roster node | `reac_roster.c/.h`, `reac_roster_node.c/.h`, `main.c:4960-5060` | Restart rebuild is correct. L8 |
| Config and knobs | `reac_knobs.c/.h`, `reac_segconf.c/.h`, `reac_role_cfg.c/.h`, `reac_rate_cfg.c/.h`, `reac_declared_vlan.c/.h`, `reac_box_pin.c/.h` | M5, M11, L13, L14 |
| Head-amp and gain | `reac_headamp_prop.c/.h`, `reac_headamp_state.c/.h`, `reac_gain.c/.h`, `reac_lat.c/.h`, `reac_watch.c/.h` | Bounded and allocation-free. L13 (float cast) |
| Docs vs code | `README.md`, `docs/*.md`, `docs/design/**` | M14, L22, L23, L24 |
| Tests and CI | `meson.build` test registrations, `tests/*.sh` (netns), `test_reac_node_recover/ensure/qdisc/sink_format/source_format/knob_table/watch/segconf.c`, `asserted-lines-have-a-producer.py`, `reac-code-conformance.py`, `.github/workflows/release-rpm.yml`, release job log 107802347856 | H3, M13, L19-L21 |
| libreac (out of scope; cross-checks only) | `transport/src/reac_linkmon.c`, `reac_handle.c`, `reac_conf.c`, `reac_rx.c`, `reac_seglock.c` at `ee205b6` | Used to confirm M5 and M6 and to discard the fd 0 close |
