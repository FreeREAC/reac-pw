# The fuzz harnesses of reac-pw

reac-pw parses untrusted input of its own, beside the REAC frames libreac parses: the
operator's configuration, the control writes a PipeWire client sets on a node, and the
rtnetlink replies of the kernel. Each reader of that input has one libFuzzer harness
here. libreac's own readers (the frames, the control blocks, the identity pages, the
discovery) are libreac's to fuzz, and its harnesses do: no harness here drives them.

| harness | reader | what it parses |
|---|---|---|
| `fuzz_segconf` | `src/reac_segconf.c`, `src/reac_declared_vlan.c` | `reac-pw.conf` and its drop-ins, and the VLAN segments they declare |
| `fuzz_pin` | `src/reac_box_pin.c` | the `--box MODEL[:LABEL]` pin |
| `fuzz_headamp` | `src/reac_headamp_prop.c` | the `reac.headamp.<ch>.<param>` cells of a Props write |
| `fuzz_rate` | `src/reac_rate_cfg.c` | the `reac.cfg.rate` Props write and the decision on it |
| `fuzz_role` | `src/reac_role_cfg.c` | the `reac.cfg.role` Props write and its answer |
| `fuzz_qdisc` | `src/reac_qdisc.c` (`reac_qdisc_stats_read`, `reac_qdisc_etf_catchup_slots`) | the RTM_GETQDISC dump of the etf counters |
| `fuzz_link` | `src/reac_link_alias.c`, `src/reac_served_mark.c` | the RTM_GETLINK reply of an alias and link kind, the RTM_NEWLINK that writes one, and the marking policy over them |

Each harness's header says how it reads its input, where its expectations come from and
which properties it checks.

## The rules every harness keeps

- **Each property is an `if` that calls `abort()`**, in a `noinline` function
  `check_<property>`, never an `assert()`. Its comment says what it checks and names, in
  `contract:` lines, the header, spec or uapi definition the expectation comes from.
- **The expected value is the harness's own.** It is worked out from the input by the
  grammar the reader documents, never read back from the reader or from a field the
  reader writes: the config file is read again line by line in `fuzz_segconf.c`, the
  Props object is written by `fuzz/pod.h`, which keeps a record of what it wrote, and the
  netlink replies are walked by the grammar of `fuzz/netlink.h`. A limit or a constant the
  expectation needs is defined in the harness, with the line of the header or spec that
  states it.
- **Each check names the bug it catches.** Its comment carries one or more records,
  a one-line change of `src/` and the seed it shows on:

      mutant: <path>:<line> `<old>` -> `<new>`
      input: <seed of fuzz/corpus/<harness>/>

  or one line `not a property: <reason>` for a stop after the harness's own allocation
  failed. `make -C fuzz fuzz-records` makes each change alone and requires exit 134 from
  that check's `abort()` on that seed, and exit 0 without the change. The patches of
  `fuzz/sabotage/` are the same kind of breakage, each a whole patch whose two header
  lines name the check and the seed (OSS Verify's `mutants` directory).
- **Every check is reached.** `make -C fuzz fuzz-coverage` requires at least one seed to
  run each check, and each harness to cover at least its floor of reader lines
  (`fuzz/coverage-floor.txt`).
- **A run depends only on its input**: no clock, environment, thread, file or network.
  The netlink harnesses rename the readers' socket calls to the harness's fake kernel
  (`fuzz/build.sh`), and `fuzz_segconf` drives the text parser, not the file loader.
  Inputs of 64 MiB or more are refused, as the OSS Verify driver refuses them.
- **A run writes nothing of its own.** What a harness writes is no part of what it
  checks, and the output of a run belongs to its driver. `fuzz_link` drives a policy that
  reports a mark it could not write, and a netdev it left alone
  (`src/reac_served_mark.c`): the harness points that policy's stream
  (`reac_served_say_to`) at a buffer in memory, so it decides and formats exactly as it
  does in the daemon and the run writes nothing.
- **No harness links libreac**, only its headers. The box-model table the config reader
  and the pin look tokens up in is `fuzz/catalogue.c`, the rows of libreac 1.8.0;
  libreac's harnesses prove its own tables.

## What has no harness, and why

- **The file loader of `reac_segconf.c`** (`reac_segconf_load`, `reac_segconf_refresh`,
  the drop-in directory walk): it opens files and reads their stamps, which a harness may
  not do. Its parse, the part that reads the bytes, is `fuzz_segconf`'s.
- **The knobs (`src/reac_knobs.c`)**: a knob is read from the environment, the command
  line and libreac-transport's env files (`reac_conf_lookup`). The environment is not an
  input a harness may read, the file grammar is libreac-transport's, and what reac-pw adds
  is a table lookup with no parse of its own.
- **The qdisc policy (`reac_qdisc_arm`, `_disarm`, `_release`)**: it acts on the answers
  of libreac-transport's etf calls, not on bytes. `fuzz_qdisc` links stand-ins for those
  calls only so the file links; it never calls the policy.
- **The topology tap of `src/main.c`** (`topo_tap_read`): the frame it reads is classified
  by libreac's `reac_topo_classify`; what reac-pw itself reads there is the ifindex and the
  packet type of `struct sockaddr_ll` and the VLAN tag of the auxdata, all from the
  kernel's own socket layer, in a static function of the daemon's main loop.
- **The PipeWire nodes** (`reac_sink_node.c`, `reac_source_node.c`, `reac_roster_node.c`):
  they hand the Props objects to the three Props readers above, which are fuzzed, and
  otherwise run against a live PipeWire graph.
- **The box row (`src/reac_box_row.c`)**: its inputs are the widths and the identity page
  libreac has already parsed out of the box's frames, and the name is libreac's
  `reac_box_name`; it reads no bytes itself.
- **The decision cores with no input bytes** (`reac_wake.c`, `reac_watch.c`,
  `reac_node_recover.c`, `reac_lat.c`, `reac_link_budget.c`, `reac_gain.c`,
  `reac_roster.c`, `reac_headamp_state.c`): their inputs are clocks, counters and states
  the daemon computes, not bytes from outside; their unit tests in `tests/` hold them.

## Running them

The harnesses need clang with libFuzzer, llvm-cov, the static libc, and the libreac,
libreac-transport and SPA headers (`dnf install clang compiler-rt llvm glibc-static
pipewire-devel libreac-devel libreac-transport-devel`). From the top of the tree:

    make -C fuzz fuzz                    # every harness, with ASan and UBSan
    make -C fuzz fuzz-run FUZZ_TIME=60   # fuzz each from its corpus
    make -C fuzz fuzz-records            # every record and sabotage patch, alone
    make -C fuzz fuzz-sabotage           # every patch found by the corpus, in its check
    make -C fuzz fuzz-coverage           # every check reached, every floor held
    make -C fuzz fuzz-static             # each harness linked fully static
    make -C fuzz fuzz-corpus-check       # the corpus is what fuzz/gen-corpus.c writes

The seed corpus is binary, written by `fuzz/gen-corpus.c` (`make -C fuzz fuzz-corpus`)
and committed; a seed's name is what the records' `input:` lines name. An input that
crashes `fuzz-run` is left in `fuzz/findings/`; to reproduce it, run the harness on it:
`build/fuzz/fuzz_<harness> fuzz/findings/<file>`.
