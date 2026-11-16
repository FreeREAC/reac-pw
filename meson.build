# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# reac-pw — PipeWire-native REAC endpoint (RX source node; TX sink scaffold).
#
# Reuses, does not reinvent:
#   - libreac  RX core: validate / counter / rate-detect + decode / capture / pcap
#              (<reac/*.h>; system libreac-devel, or the bundled subproject fallback)
# and links against the system libpipewire-0.3 / libspa-0.2 (Fedora pipewire-devel).

project('reac-pw', 'c',
  version: '1.0.43',
  license : 'GPL-3.0-or-later',
  default_options : ['c_std=c11', 'warning_level=2', 'buildtype=debugoptimized'])

cc = meson.get_compiler('c')

# SPA's inline string.h uses newlocale()/uselocale()/locale_t (LC_ALL_MASK),
# which are GNU/XPG extensions — define _GNU_SOURCE project-wide so every TU that
# includes a PipeWire/SPA header gets them (PipeWire itself builds this way).
add_project_arguments('-D_GNU_SOURCE', language : 'c')

# --- system deps: PipeWire + SPA (the only external libs) -------------------
pipewire_dep = dependency('libpipewire-0.3', version : '>=0.3.65')
spa_dep      = dependency('libspa-0.2')
threads_dep  = dependency('threads')
# libm only needed by the decode core's siblings; harmless if unused.
m_dep        = cc.find_library('m', required : false)

# --- libreac: RX/measure core, the installed package ----------------------
# libreac-devel (RPM) / libreac-dev (deb) from the FreeMixer channel, found by pkg-config and
# linked dynamically. The floor below is also written, with a ceiling, in packaging/reac-pw.spec
# and debian/control (tests/libreac-is-a-bounded-package-dependency.sh keeps them so); nothing of
# libreac is fetched, cloned or bundled. The header is <reac/reac.h>.
# >=0.5.0: the braid/sample layout oracles, the upstream decode, the OHRCA
# clean-len rule, the ENCODE side (since 0.4.0 — reac_braid_encode +
# reac_downstream_build) AND, since 0.5.0, a reac_decode() that reads the SAME
# braid its own encoder writes. This version floor is load-bearing twice over:
# reac-pw no longer carries its own encoder, so a 0.3.x libreac must fail at
# configure time rather than at link time; and a 0.4.x libreac would still LINK
# while decoding the downstream as plain LE, so the RX path would silently
# disagree with the TX path about the wire layout (#80). Building quietly
# against an older system libreac is exactly what this floor exists to stop.
#
# >=0.7.0 IS THE FLOOR THAT MATTERS NOW, and the one that had to be raised by
# hand because the previous one could not fail. libreac 0.7.0 removed
# reac_ctrl_build_name_frame() and reac_ctrl_build_extra_frame() in favour of
# reac_ctrl_build_identity_first(), which is what src/ calls. That break first
# shipped from libreac as 0.6.0 with soname 0 unchanged, so this floor read
# `libreac found: YES 0.6.0` for a library with and without the symbols alike,
# and the mismatch surfaced only when the installed binary hit `undefined
# symbol` at exec. A floor that accepts both sides of an API break is not a
# floor. Raising it to 0.7.0 makes the wrong library fail at CONFIGURE.
#
# >=1.1.0 IS THE FLOOR THAT MATTERS NOW, raised by hand for the same reason and
# to the same rule. src/reac_sink_node.c calls reac_box_identity_publish(),
# which libreac 1.1.0 adds; and struct reac_identity, which this build
# allocates through struct reac_pacer, went 30 -> 38 bytes there. A 1.0.x
# libreac satisfies '>=1.0.1' and then fails at link on the missing symbol --
# or, worse, links against a 30-byte identity and is written 38 bytes of. The
# floor is what makes the wrong library fail at CONFIGURE.
# >=1.1.3 SINCE 1.0.7: <reac/reac_link_state.h> declares REAC_PROP_PACE_BACKEND and
# REAC_PROP_PACE_REFUSAL, which reac_sink_node.c publishes. A 1.1.2 satisfies '>=1.1.0'
# and fails at COMPILE on the missing macro -- which is a cheap failure, but the floor
# should still say what is actually needed rather than what used to be.
# 1.1.5 is the floor: reac_link_admin() — the PHY edge reac_wake asks for — arrives there,
# and a 1.1.4 would configure clean and fail at LINK, which is the shape of failure these
# hand-raised floors exist to turn into a configure-time refusal.
# 1.2.1 IS THE FLOOR, and it is a BEHAVIOUR floor rather than a symbol one — the first
# of those, so it says why. libreac 1.2.1 is what parses the S-4000H's declaration
# (input groups marked 0x00, outputs written first), carries its captured row and
# answers one verdict per MAC. Built against 1.2.0 this daemon COMPILES AND LINKS
# perfectly and then cannot enrol that box at all: the port decoder refuses the table,
# the master never sizes it, and the roster sits at probing — which is precisely the
# live defect of 2026-09-17. A floor that only ever catches missing symbols would let
# that ship, so this one is raised for what the library DOES.
# 1.2.2 RAISES IT AGAIN, for behaviour again: that is where reac_topo_tap_open() creates
# the topology tap DEAF (protocol 0) and lets the bind make it live, so the tap can no
# longer hear another interface's frames in the microseconds before it is bound (libreac
# #18, measured at 88 632 foreign frames over 400 opens on 1.2.1). Against 1.2.1 this
# daemon builds and runs and misclassifies a cold parent as a trunk on start, which is
# the defect of 2026-09-14 — invisible to a symbol floor.
# >=1.4.0 SINCE 1.0.23: <reac/reac_knock.h> and <reac/reac_tapwait.h> moved into libreac
# (operator ruling 2026-09-22 — deciding what a wire is belongs to the library; reac-pw
# deals with enrolled nodes). Against an older libreac this build dies at main.c's
# #include, which is a loud enough failure; the floor is raised so it arrives at configure
# time instead, and so tests/test_reac_enrolment_binding.c's own #error has a partner.
# >=1.5.0 SINCE 1.0.24: REAC_TOPO_TAGGED_OTHER (a tag on any ethertype names a VLAN, which
# is how a COLD one is found) and reac_topo_heard_vids() are 1.5.0 surface, and this daemon
# reads both. An older header does not compile; the floor turns that into a configure-time
# refusal.
# >=1.5.1 SINCE 1.0.27: a box is granted at any width it declares, up to 40. A 1.5.0 grant
# refuses a 40 in / 0 out box, so it is never enrolled and never asked for its identity;
# every symbol is there, so only the floor turns that into a configure-time refusal.
# >=1.7.0 SINCE 1.0.30: every box fact comes from its frames (FreeREAC/reac-pw#5):
# reac_box_facts.h (the family off the hw block, the derived name), the model catalogue
# rename, the declared widths in discovery, the identity re-poll and the S-0808 name
# reassembly. An older header does not compile.
# >=1.9.0: enum reac_rate_refuse, enum reac_role_refuse, their REAC_*_REFUSE_CODES_INIT
# tables, REAC_RATE_BIT_* / REAC_RATE_ALL_BITS and REAC_ROLE_STATE_APPLIED / _REESTABLISH_PENDING
# are declared in <reac/reac_cfg.h> (FreeREAC/libreac#8); src/reac_rate_cfg.h and reac_role_cfg.h no
# longer define them, so an older header does not compile.
# >=1.10.0 SINCE 1.0.42: ONE HOME (libreac's 2026-09-29-shared-code-has-one-home). <reac/reac_code.h>
# is the one code list (src/reac_code.h is gone; 1.10.0 is the first release declaring every token this
# daemon emits), and reac_etf_qdisc_stats_read is the one RTM_GETQDISC counter read (src/reac_qdisc.c's
# copy is gone). An older header does not compile.
# >=1.11.0 SINCE 1.0.43: libreac.so.7 (struct pcap_source grew); the packages carry the ceiling.
libreac_dep = dependency('libreac', version : '>=1.11.0', required : true)

# libreac-transport (2026-09-11-reac-transport-library): the sockets,
# SCHED_FIFO pacer, RT threads, VLAN/topology scan, ring and segment lock that used to be
# compiled here as src/*.c now live in this library, unchanged. # -- a machine without pkgconfig(libreac-transport) simply fails to configure, same as any
# other required system dependency.
# >=1.1.0: struct reac_pacer embeds libreac 1.1.0's grown reac_identity, so it went
# 24304 -> 24312 bytes and its soname moved to libreac-transport.so.4. This build
# allocates a reac_pacer, so an older transport is the same size mismatch as above.
# >=1.1.3 SINCE 1.0.7: reac_etf_qdisc_install/_remove/_state/_fix (the qdisc the daemon
# owns) and reac_pacer_backend_refusal are 1.1.3 symbols, and 1.1.3 is also where the
# pacer's default became etf. A 1.1.2 satisfies '>=1.1.0', links against nothing that is
# missing at configure time, and fails at LINK -- which is exactly the shape of failure
# these floors are hand-raised to turn into a configure-time refusal.
# >=1.2.2 SINCE 1.0.18: reac_topo.c lives in this library, and 1.2.2 is where its tap
# stops hearing unbound-window traffic (libreac #18). Behaviour, not symbols, so nothing
# below configure time would catch a 1.2.1.
# >=1.5.0 SINCE 1.0.24: the tap's BPF is what admits a tagged non-REAC frame at all. A
# 1.2.x transport has every symbol this daemon links and simply never hears a cold VLAN --
# behaviour, not symbols, and the exact shape of failure these hand-raised floors exist for.
# >=1.5.1 SINCE 1.0.27: struct reac_pacer grew with the 40-wide grant sweep (soname 6), and
# this build allocates one.
# >=1.10.0 SINCE 1.0.42: reac_etf_qdisc_stats_read's dump walker stops at the bytes recv() returned
# (an unpadded last message no longer walks the stale buffer); the health line reads through it.
libreac_transport_dep = dependency('libreac-transport', version : '>=1.11.0', required : true)

# A sibling libreac checkout's transport/src — dev-only, for the two spots that need the
# door's actual SOURCE rather than its installed header (bench_hotpath's #include trick,
# rt_single_door's positive control). Empty by default; both spots degrade honestly
# (not-built / SKIP) rather than faking a result without it.
libreac_transport_srcdir = get_option('libreac_transport_srcdir')

# The floor bites on BOTH resolution paths, verified when it was raised:
#   - system libreac-devel 0.2.0 -> rejected by pkg-config, fallback taken;
#   - a stale subprojects/libreac left over from a 0.4.0 build -> rejected too,
#     because the wrap patches the version-carrying shim in at clone time, so an
#     old checkout reports its own old version. `meson subprojects update`, or
#     just remove subprojects/libreac and reconfigure.
# The RPM (--wrap-mode=nofallback) has no fallback to take, so its floor is the
# spec's BuildRequires: pkgconfig(libreac) >= 0.7.0 — keep the two in step.
# (That line said 0.5.0 while the spec said 0.6.0: the reminder to keep them in
# step was itself out of step, which is why the number now appears in a comment
# that is checked against the spec whenever either moves.)

# The REAC decode / capture / pcap core comes from libreac via
# <reac/reac_decode.h> etc., and since 0.3.0 so do the braid codec
# (<reac/reac_braid.h>), the f32<->s24 pair (<reac/reac_sample.h>) and the
# box-upstream decode (<reac/reac_upstream.h>). Since 0.4.0 the ENCODE side is
# there too (<reac/reac_encode.h>): reac_downstream_build builds the 1492 B
# master broadcast and reac_braid_encode lays the braided audio region of the
# box->master frames reac_ctrl builds. Since 0.5.0 both RX directions read that
# one braid — reac_decode for the downstream, reac_upstream_decode for the box
# return — so the encoder and the decoder in this binary agree by construction.
# reac-pw keeps only what libreac will not
# do — AF_PACKET emission (reac_tx.c), the SCHED_FIFO pacer, and the
# control-block/checksum handshake. No local copies of any layout.

# --- the reac-pw client -----------------------------------------------------
reac_pw_src = [
  'src/main.c',
  'src/reac_knobs.c',
  'src/reac_box_pin.c',
  'src/reac_rx_bind.c',
  'src/reac_watch.c',
  'src/reac_wake.c',
  'src/reac_knock_wire.c',
  'src/reac_link_owner.c',
  'src/reac_empty_master.c',
  'src/reac_node_recover.c',
  'src/reac_headamp_prop.c',
  'src/reac_headamp_state.c',
  'src/reac_rate_cfg.c',
  'src/reac_role_cfg.c',
  'src/reac_sink_format.c',
  'src/reac_node_ensure.c',
  'src/reac_node_graph.c',
  'src/reac_source_node.c',
  'src/reac_sink_node.c',
  'src/reac_knock_credit.c',
  'src/reac_box_row.c',
  'src/reac_lat.c',
  'src/reac_gain.c',
  'src/reac_qdisc.c',
  'src/reac_link_alias.c',
  'src/reac_served_mark.c',
  'src/reac_link_budget.c',
  'src/reac_declared_vlan.c',
  'src/reac_segconf.c',
  'src/reac_roster.c',
  'src/reac_roster_node.c',
]

# THE PROTOCOL'S NUMBERS COME FROM ONE DECLARATION (src/reac_facts_pw.h, tools/facts.py).
# facts/ holds the verbatim copy of reac-protocol's generated reac_facts.h (plus the
# reac_facts_undef.h derived from it); the `facts_dir` option swaps in another copy,
# which is how tools/facts-perturb-check.sh builds against a perturbed fact set.
facts_dir = get_option('facts_dir')
if facts_dir == ''
  facts_dir = 'facts'
endif
src_inc = [include_directories('src'), include_directories(facts_dir)]

# include_directories('src') is new here: it used to resolve implicitly (a quoted
# #include finds a sibling in the SAME directory as the including file first, and
# main.c/reac_sink_node.c's own local headers always were siblings). Now that
# reac_pacer.h / reac_role_swap.h moved to libreac-transport and still #include
# reac-pw's reac_rate_cfg.h / reac_role_cfg.h for pure declarations (the design
# spec's own named seam), that quoted include is no longer beside the including
# file and needs the -I fallback instead.
reac_pw_exe = executable('reac-pw',
  reac_pw_src,
  include_directories : src_inc,
  dependencies : [pipewire_dep, spa_dep, threads_dep, m_dep, libreac_dep, libreac_transport_dep],
  install : true)

# --- source sweep: ONE DOOR TO SCHED_FIFO ------------------------------------
# The ladder above only holds while reac_rt.c is the only place that changes a
# thread's scheduling class; a third RT thread spelling its own literal would
# reopen #31 with every unit test still green. The sweep proves it can see the
# call in the file that legitimately has it before it reports absence anywhere.
rt_single_door_args = [meson.current_source_dir()]
if libreac_transport_srcdir != ''
  rt_single_door_args += libreac_transport_srcdir
endif
test('reac_rt_single_door',
  find_program('tests/rt-prio-single-door.sh'),
  args : rt_single_door_args)

# --- source sweep: NO PINS (reac-pw#23) --------------------------------------
# libreac is declared in the spec and debian/control as a floor and a ceiling and nowhere else.
test('libreac_is_a_bounded_package_dependency',
  find_program('tests/libreac-is-a-bounded-package-dependency.sh'),
  args : [meson.current_source_dir()])

# --- unit test: the RX stream gate (downstream vs upstream accept) ------------
# Runs the real feeder thread over a temp pcap that interleaves both streams
# (synthetic 40-ch broadcast + the captured box return + a second box).
# It also runs the slave side's lock to its desk through the same feeder
# (tests/rx_bind_feeder.inc, src/reac_rx_bind.c): a forger beside the desk is decoded
# unlocked (the control) and counted, not decoded, once the gate is locked.
rx_gate_test = executable('test_reac_rx_gate',
  ['tests/test_reac_rx_gate.c', 'src/reac_rx_bind.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep, threads_dep, m_dep])
test('reac_rx_gate', rx_gate_test)

# --- unit test: the slave side's receive gate is locked to its desk -------------
# The learned-MAC rule of the two calls main.c makes (src/reac_rx_bind.c); the feeder
# arms (forger beside the desk, unlocked control) run in test_reac_rx_gate.
rx_bind_test = executable('test_reac_rx_bind',
  ['tests/test_reac_rx_bind.c', 'src/reac_rx_bind.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep, threads_dep, m_dep])
test('reac_rx_bind', rx_bind_test)

# --- integration: a lost --live interface must TERMINATE the daemon ----------
# The unit test above pins the PREDICATE; this drives the whole binary and
# checks the consequence, because detection without exit is not recovery (the
# 2026-08-29 S-0808 outage: the daemon knew, said so, and kept running with a
# dead socket while the operator replugged a healthy box). Two scenarios, the
# second being the one a name-only check cannot see: the interface returns
# under the SAME NAME with a new ifindex.
#
# Runs inside an unprivileged user+net namespace on its own dummy interfaces,
# so it cannot perturb a live REAC segment; skips where that or PipeWire is
# unavailable. Timeout covers 2 x (5 s settle + 6 s observe) plus startup.
test('reac_iface_vanish_exits',
  find_program('tests/iface-vanish-exits.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe],
  timeout : 60)

# --- whole-binary test: a cable edge re-establishes the master (#95) ---------
# The unit tests prove the watcher reports edges and the pacer re-establishes
# when asked. Neither proves the lines that JOIN them — the netlink fd on the
# main loop, the callback, the request across to the pacer thread — which is
# exactly the path a green suite hides: every part works, nothing calls
# anything. This drives the real binary against real `ip link` toggles and
# requires the FSM transcript to attribute the transition to the WIRE.
# Own dummy interface inside an unprivileged user+net namespace, so it cannot
# perturb a live REAC segment. Timeout covers 5 s settle + 3 s of toggles.
test('reac_link_up_reestablishes',
  find_program('tests/link-up-reestablishes.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe],
  timeout : 60)

# A STAGEBOX WITH ITS REAC MODE SWITCH ON M, for the far end of the veth pair. The daemon
# cannot play one (its master role IS a desk: the fixed 40-channel downstream), and the
# 0.5.1 ruling turns on exactly this peer -- joined on an unpinned wire, refused on one
# pinned master. Same libreac builders as test_reac_hunt's own fixture.
fake_box_master = executable('fake-box-master',
  ['tests/fake_box_master.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep, m_dep],
  install : false)

# --- whole-binary test: the daemon OWNS the qdisc on the device it binds ------
#
# ETF is the default backend (2026-09-14). The qdisc and the backend are ONE setting, and
# with skip_sock_check an etf qdisc DROPS every frame carrying no launch time -- so the
# rig has already met both failures: an etf daemon with no qdisc stamping launch times
# nobody honours, and a thread daemon under a leftover etf qdisc transmitting 0 packets in
# a 60 s window. This drives the real binary over a veth in a private namespace and counts
# REAC frames ARRIVING AT THE FAR END, because a qdisc eating the wire looks exactly like a
# healthy daemon from the inside. Its own instrument (tests/etf_wire_probe.c) reads the
# qdisc through the library's public netlink door, never by scraping `tc`.
etf_wire_probe = executable('etf-wire-probe',
  ['tests/etf_wire_probe.c', 'src/reac_qdisc.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep],
  install : false)

test('reac_etf_qdisc_owned',
  find_program('tests/etf-qdisc-owned.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, etf_wire_probe],
  timeout : 300)

# --- unit test: what counts as a DECLARED VLAN segment ------------------------
# Pure text: the key/filename forms an operator writes, the two that look like VLANs and
# are not (`reac-pw.env`, `enp131s0.env`), and the bounds. No netlink, no netdev.
declared_vlan_test = executable('test_reac_declared_vlan',
  ['tests/test_reac_declared_vlan.c', 'src/reac_declared_vlan.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep])
test('reac_declared_vlan', declared_vlan_test)

# --- unit test: the ONE override file's grammar, and what it REFUSES -----------
# Pure text plus one file this test writes. Every refusal is asserted to NAME its subject:
# a config reader that drops a line in silence is how an operator ends up with a rig that
# is not doing what their file says -- which, one level up, is the defect the whole
# autodetect spec exists to remove.
segconf_test = executable('test_reac_segconf',
  ['tests/test_reac_segconf.c', 'src/reac_segconf.c', 'src/reac_declared_vlan.c',
   'src/reac_knock_wire.c', 'src/reac_wake.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep])
test('reac_segconf', segconf_test)

# --- unit test: the ROSTER's prop grammar and its DELTA ------------------------
# The second amendment took the zero-port node off the graph and NAMED what it cost: an
# empty segment has no row on the console at all. This node is that row, and the subject
# here is the DELTA -- a roster that republishes every tick is a property storm on every
# client with the same node id, which is the defect one door down from the node churn the
# node exists to avoid. Pure: no PipeWire, no netlink, no clock.
roster_test = executable('test_reac_roster',
  ['tests/test_reac_roster.c', 'src/reac_roster.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep])
test('reac_roster', roster_test)

# --- whole-binary test: segments and roles are AUTODETECTED -------------------
#
# The 2026-09-16 desk defect: the rig moved from one box on a 100 Mbit USB NIC to three
# boxes on a 1 Gbit trunk and the console's GENERATED reac-pw.env still carried the
# previous rig's answers -- the VLAN segments pinned `tap`, so the daemon served three
# mirror ports on a fabric with no mirror, every node up and no audio moving. From inside
# the daemon a stale file and a correct one are indistinguishable. test_reac_segconf pins
# the grammar and every rule in it can be right while main.c still asks reac_conf for
# REAC_ROLE, so this drives the real binary against real netdevs: a stale env deciding
# nothing, a conf overriding one segment and removing another, a tagged VLAN id with no
# sub-interface named by its id, and an interface hot-plugged under a running daemon.
test('reac_segments_autodetect',
  find_program('tests/segments-autodetect.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe],
  timeout : 300)

# --- whole-binary test: a DECLARED VLAN waits for its tag (operator ruling 2026-09-23) ---
#
# "We don't carry any VLANs if we don't detect VLANs." Until 2026-09-23 a declared
# `<parent>.<vid>` was minted at start; the desk paid three vacant tap doors per boot on a
# parent that hears no tag. (The cold door since #99 is a different sentence, `[trunk
# <parent>]`, proven by reac_trunk_vlans_declared_cold below; this rule stands for
# [segment].) Now the real binary, in a private user+net+pid namespace, must
# mint NOTHING for two declared VIDs on a present parent in four seconds of silence, leave a
# host-made sub-interface alone, mint the declared VID the moment libreac's fake box master
# transmits inside it on the far end (the kernel tags every frame), report the declared role
# on it, and remove on exit only what it minted. Red on 1623184, which minted both at once.
test('reac_declared_vlan_waits_for_its_tag',
  find_program('tests/declared-vlan-waits-for-its-tag.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, fake_box_master],
  timeout : 300)

# --- unit test: the ETF catch-up budget, "unreadable is not zero", and the qdisc record --
# Pure arithmetic plus one rtnetlink dump of the host's own qdisc table (a read). The
# record cases (#109: a refused install is never "just installed"; our own qdisc whose
# removal failed stays on record for the exit) run the real reac_qdisc.c policy over a
# SCRIPTED kernel: the three libreac-transport doors it calls are wrapped at link time,
# so no device is touched and a failing removal can be arranged without privileges.
qdisc_test = executable('test_reac_qdisc',
  ['tests/test_reac_qdisc.c', 'src/reac_qdisc.c'],
  include_directories : src_inc,
  link_args : ['-Wl,--wrap=reac_etf_qdisc_state',
               '-Wl,--wrap=reac_etf_qdisc_install',
               '-Wl,--wrap=reac_etf_qdisc_remove'],
  dependencies : [libreac_dep, libreac_transport_dep])
test('reac_qdisc', qdisc_test)

# --- unit test: the served-interface mark (the alias the NetworkManager drop-in is made from)
# The real reac_served_mark.c policy over a SCRIPTED kernel: the two rtnetlink doors are
# wrapped at link time, so no device is touched.
served_mark_test = executable('test_reac_served_mark',
  ['tests/test_reac_served_mark.c', 'src/reac_served_mark.c'],
  include_directories : src_inc,
  link_args : ['-Wl,--wrap=reac_link_alias_read',
               '-Wl,--wrap=reac_link_alias_write'],
  dependencies : [libreac_dep])
test('reac_served_mark', served_mark_test)

# --- whole-binary test: under ETF, `late` is the THREAD'S WAKE, not the wire --
#
# Since ETF became the default the health line read 19-41 late/s on a desk whose WIRE
# was exact to 1.1 us of stddev. Both are true and they are not the same measurement:
# the pacer thread sleeps to `launch - lead` and the KERNEL releases the frame, so a
# wake up to lead-minus-delta late costs nothing. This drives the real binary over a
# veth in a private namespace, three arms under one load, and measures the wire with
# libreac's own pace_hist over a tcpdump taken at the FAR END — the daemon's own
# figure is the thing on trial, so it cannot also be the evidence.
#
# The arm that makes it a proof is the POSITIVE CONTROL: the same daemon with the
# lead shrunk under the thread's wake tail, where launch times really are missed. The
# qdisc's drop counter moves an order of magnitude; `late` does not move at all. A
# counter that reads the same when the wire is exact and when it is broken is not the
# counter to put in front of an operator.
#
# Needs libreac's pace_hist (REACPW_PACE_HIST) for the wire arm; without it the wire
# assertions are announced as NOT RUN rather than skipped quietly.
test('reac_etf_late_is_the_wake',
  find_program('tests/etf-late-is-the-wake.sh'),
  suite : ['netns', 'load'],
  is_parallel : false,
  args : [reac_pw_exe, etf_wire_probe],
  timeout : 600)

# --- whole-binary test: a bare daemon HEARS a segment, keeps it through a flap,
# drops it past the hold, hears it again (trunk-VLAN spec, amendment 2026-09-02).
# test_reac_ifscan pins the table; this drives the real binary on a veth pair
# with a second reac-pw mastering the peer end, inside an unprivileged user+net
# namespace. Skips where that or PipeWire is unavailable. Timeout covers
# 3 + 4 + 3 + 4.5 + 5 s of waits plus two startups, and since 2026-09-08 the three
# cold-start phases (pinned-drives-on-link, the knock, a desk answering) with their own
# peer captures and a 6 s no-further-announce hold.
test('reac_hearing_finds_a_segment',
  find_program('tests/hearing-finds-a-segment.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, fake_box_master],
  timeout : 600)

# --- whole-binary test: a box that DROPPED is re-acquired by the daemon alone ------
#
# The 2026-09-16 desk defect: after a 77-minute s2idle the daemon probed a wire correctly for
# 73 minutes across two processes (~1620 completed scene pushes, 8003 frames a second out,
# rx_box_frames=0) while an enrolled S-1608 that had dropped sat silent. Every unit test in
# this repo was green throughout, because each asks whether a PART behaves and what had to
# behave was the daemon acting on its own silence. So this drives the real binary against
# libreac's fake_box under FAKE_BOX_DROPPED=1 — a box that receives whole transfers, counts
# them and answers none until its own carrier goes and returns, which is its decompiled FSM
# and not our opinion. Its second arm is the control: the same box with the mode off enrols
# on the push alone and the daemon must NOT bounce it. fake_box lives in the other repo, so
# the path is a knob; without it the test SKIPS rather than passing while proving nothing.
fake_box_path = get_option('fake_box')
# --- whole-binary: the knock's answer is the master's cue, and is granted as handed over (#58)
# libreac's fake_box is the far end: silent until a COMPLETE push, like the rig's boxes, and
# started before the daemon. Three bars, each red on 1.0.39: the master takes the wire within
# 2 s of ANSWERED (not after the hunt's 3 s window), the carried answer takes the edge as it is
# ingested ("(rx CONFIG)", not a "(timer)" release after a second push), and it establishes.
test('reac_knock_answer_is_the_cue',
  find_program('tests/knock-answer-is-the-cue.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, fake_box_path],
  timeout : 120)

# --- whole-binary: the trunk's VLANs, declared once, are minted COLD and knocked on (#99) ---
# A trunk parent finds its VLANs by hearing tagged frames; a cold box says nothing until a
# master speaks and a box on an access port never hears the parent's untagged knock, so with
# every box cold nothing is ever found. The owner's ruling on #99 (2026-11-15): the trunk's
# VLAN ids declared once, `[trunk <parent>]` / `vlans = ...`, each minted at start and
# knocked on in full. The far end is libreac's fake_box behind a VLAN made mute (no IPv6
# chatter to name it), started before the daemon: trunkA.11 and .33 exist, up and marked,
# before any tagged frame; 11 is knocked, answered and established; 33 is knocked, never
# answered, and stays; a VID nobody declared is not minted; a late parent is minted when it
# appears; the exit removes exactly what was minted. REACPW_TEST_SABOTAGE=no-list drops the
# list and turns the test red. Red on 224a73c (main before #99), with nothing tagged heard.
test('reac_trunk_vlans_declared_cold',
  find_program('tests/trunk-vlans-declared-cold.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, fake_box_path],
  timeout : 300)

test('reac_box_wakes_on_a_phy_edge',
  find_program('tests/box-wakes-on-a-phy-edge.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, fake_box_path],
  timeout : 240)

# --- whole-binary: frames the kernel refuses HOLD the ladder, said once (2026-09-23) ------
#
# The boot of 2026-09-23 13:43: the etf root qdisc the daemon had installed for the ETF
# default stayed on the device when libreac refused ETF (TAI offset 0 before chrony), so
# every thread-backend frame was dropped — tx=0, tx_errors=8000/s — while the master counted
# "COMPLETED" pushes and the ladder spent two PHY edges on a box that had never heard us.
# No box needed: the refusal is about us. The held arm adds the qdisc by hand under a
# thread-backend daemon and requires NO edge in 40 s plus the refusal said exactly once;
# the control arm is the same empty wire with frames leaving, which must be bounced.
test('reac_wake_holds_when_nothing_is_sent',
  find_program('tests/wake-holds-when-nothing-is-sent.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe],
  timeout : 240)

# --- whole-binary: a NIC the host uses HOLDS the ladder, said once (#46, 2026-10-26) ------
#
# The desk 2026-10-26 19:19Z: the ladder took the trunk parent down for 1200 ms, the NIC
# with the management address and every REAC VLAN, and every browser tab on the host
# dropped every stream. The held arm puts an address on the daemon's own veth before it
# starts and requires NO edge in 40 s, the host's-NIC refusal said exactly once with the
# kernel's reason, and frames leaving (so the hold is this refusal's, not NOTHING_SENT's);
# the control arm is the same bare veth, which must be bounced.
test('reac_wake_holds_on_a_host_nic',
  find_program('tests/wake-holds-on-a-host-nic.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe],
  timeout : 240)

# --- whole-binary: the split box enrols at its DECLARED width (2026-09-17) ---------
#
# The live defect of that morning: an S-4000H-0832 declaring 8 in / 32 out and returning
# 32-channel frames sat at `probing model=none width=0/0`. libreac's unit proof replays
# that box's captured frames; this one drives the real binary against fake_box wearing the
# same row, and reads the WIDTH OFF THE GRAPH — 8x32 — which is what a console sees and
# what no unit test in either repo can observe. Its second arm is the control: the same
# harness must read 32x8 for an S-4000S, or it cannot tell two chassis apart.
test('reac_box_0832_enrols',
  find_program('tests/box-0832-enrols.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, fake_box_path],
  timeout : 240)

# --- whole-binary: a box with no outputs has no sink, and its door is reac-capture -----
# msi 2026-10-08: an S-4000S-4000 (40 in / 0 out) stood beside a stereo Audio/Sink named
# reac-playback, "REAC segment door (no box recognized yet)". Arm 1 (fake_box fr4000) reads
# the graph's own port counts and the door's props and write answer on reac-capture; arm 2
# (s1608) is the control that shows the probe does see a sink when one exists.
# Arm 4 (tests/refuse_playback_shim.c, LD_PRELOAD) refuses an S-1608's first reac-playback,
# so reac-capture carries the door until the recovery ladder rebuilds it, and then must not.
refuse_playback_shim = shared_module('refuse-playback-shim',
  ['tests/refuse_playback_shim.c'],
  dependencies : [pipewire_dep, cc.find_library('dl', required : false)],
  name_prefix : '',
  build_by_default : true)
test('door-without-outputs',
  find_program('tests/door-without-outputs.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, fake_box_path, refuse_playback_shim],
  depends : [refuse_playback_shim],
  timeout : 360)

# --- whole-binary: the link budget yields to the segment that heard a box (#107) ------
#
# 2026-09-20, the home rig: a 100 Mbit trunk, every segment `auto`, and an EMPTY VLAN won
# the 500 ms silence race. One 96 kHz master is 97% of that port, so the admission then
# refused the only segment with a box on it -- 690 times in 30 minutes, and the S-1608
# never enrolled. test_reac_link_budget proves the arithmetic and test_reac_hunt the
# verdict; what was wrong is the ORDER two segments on one physical port reach the
# admission in, which lives in the listener table, the ifscan state machine and the
# sniffer's discovery table at once. Two VLANs on one veth, the port's rate DECLARED
# (REACPW_LINK_MBIT, since a veth reports 10 Gbit), and a negative arm in which the holder
# has a box of its own and must keep the wire.
# --- `--set KEY=VALUE` reaches the daemon at all (found by the lane above) ------------
#
# The knob resolver had a unit test; the FLAG had none, and it fell through the main flag
# loop's final `else` into usage() — so every invocation carrying the flag --help documents
# exited 2. The namespace is only how this holds CAP_NET_RAW past the preflight; it mints
# no netdev and starts no PipeWire.
test('set-flag-reaches-the-daemon',
  find_program('tests/set-flag-reaches-the-daemon.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe],
  timeout : 60)

test('empty-master-yields-the-budget',
  find_program('tests/empty-master-yields-the-budget.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe],
  timeout : 420)

# --- whole-binary: the untagged parent with a box takes the budget from an empty VLAN (#107)
#
# The rig's own geometry, which the test above leaves out (it ignores the parent): the box
# on the UNTAGGED parent, an empty `.11` that won the port first on the silence licence --
# the 20:10 ordering the rig run of the fix could not force. The port is grouped by NAME
# (link_port_of), so a veth called `lb0.11` needs no 8021q and this runs where the VLAN
# test SKIPs.
test('parent-with-a-box-takes-the-budget',
  find_program('tests/parent-with-a-box-takes-the-budget.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe],
  timeout : 240)

# --- whole-binary test: a box master is ENROLLED WITH, its own way (0.5.6) ----
#
# "It is only a matter of following the same protocol that we expect." Measured at the far
# end of the cable, against the ground-truth capture of a real S-1608 meeting the real
# S-0808: the bounded broadcast flood AT THE MASTER'S WIDTH, the config-announce BEFORE the
# cold-connect burst, the grant, the 1/s heartbeat, and a tone played into
# reac-playback.<segment> decoded off the UPSTREAM the daemon unicasts — which is what
# reaches that box's outputs. Needs a session manager (the tone has to be LINKED) and skips
# without one.
test('reac_box_master_slave_join',
  find_program('tests/box-master-slave-join.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, fake_box_master],
  timeout : 420)

# --- whole-binary test: the rejoin after a drop still carries audio ----------
#
# The 2026-09-09 13:36 rig defect: a box-master segment dropped by a link loss past the hold
# and heard again published digital silence and its create-time property seeds over a wire
# carrying 8019 frames a second. Two full cycles, and the accepted-frame count is read ONLY
# from the journal written after the drop — a feeder that accepts nothing prints no
# telemetry line at all, and the previous listener's healthy number is one `tail -1` away.
test('reac_box_master_rejoins',
  find_program('tests/box-master-rejoins.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, fake_box_master],
  timeout : 300)

# --- REAL NETLINK: the link-owner classifier reads the netdev as it is (#46) --------------
#
# The desk's trunk parent carries the management address and every REAC VLAN, and the
# knock streamed at it and bounced it (2026-10-26-a-host-nic-is-never-bounced-or-streamed-at).
# reac_link_owner says which interfaces are the host's from three rtnetlink dumps; this
# drives its probe over a veth in a private namespace, changing one fact at a time: bare,
# link-scope addresses only (still bare), an address, a VLAN child, a default route, a
# missing name. The veth's own peer naming it in IFLA_LINK is the stacked case's control.
link_owner_probe = executable('link-owner-probe',
  ['tests/link_owner_probe.c', 'src/reac_link_owner.c'],
  include_directories : src_inc,
  install : false)

test('reac_link_owner_reads_the_netdev',
  find_program('tests/link-owner-reads-the-netdev.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [link_owner_probe],
  timeout : 60)

# --- whole-binary test: a vacant tap door frees its RX ring on teardown (audit M6) --------
#
# Audit 2026-09-24, M6: a tap pinned on a silent wire falls through into the vacant door,
# which allocated L->ring through reac_rx_open, and listener_close's tap branch never freed
# it -- 5 MB per teardown at 96 kHz. The link is held down past the ifscan hold five times,
# and the daemon's VmData must stay flat across the re-opened doors.
test('reac_vacant_tap_releases_its_ring',
  find_program('tests/vacant-tap-releases-its-ring.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe],
  timeout : 180)

# --- whole-binary test: a re-purposed cable stops being a trunk (#98) --------------
#
# Measured on the rig 2026-09-13: a USB NIC classified a trunk on a switch mirror was moved
# onto a stagebox directly, and its pinned master answered `untagged REAC on a trunk's
# native VLAN is not served` until the daemon was restarted. The classification is about
# what the link carries NOW.
test('reac_trunk_classification_is_about_now',
  find_program('tests/trunk-classification-is-about-now.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, fake_box_master],
  timeout : 240)

# --- whole-binary test: one parent's tags are not another parent's verdict (#102) --
#
# Measured on the rig 2026-09-14: a USB NIC on a DIRECT cable to a cold box was told
# `tagged REAC heard — vid 11 (1 frame(s))` at every start — one frame per VLAN the daemon
# masters on ANOTHER parent — while tcpdump on it saw no tagged frame at all. The tap is
# live on every interface between socket() and bind(), so it read other links' frames as
# this parent's. Three tagged sources on one veth, a silent pinned-master veth beside it.
test('reac_trunk_verdict_is_inbound_only',
  find_program('tests/trunk-verdict-is-inbound-only.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, fake_box_master],
  timeout : 300)

# --- whole-binary test: the trunk verdict expires with no link bounce (#102) -------
#
# #98's re-proof window could only be reached by a parent that had heard NOTHING: the
# "tagged since link-up" flag was a latch, so one frame at start-up pinned the refusal for
# the life of the process. Here the tags simply stop and the link is never touched.
test('reac_trunk_verdict_expires_on_the_start_path',
  find_program('tests/trunk-verdict-expires-on-the-start-path.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, fake_box_master],
  timeout : 300)

# --- whole-binary test: a segment whose master left is re-decided (#97) ------------
#
# Measured on the rig 2026-09-13: a segment joined to a box master that then went away
# courted the empty wire for ever (ESTABLISHED -> DROP -> FLOOD_ANNOUNCE), and only a
# restart fixed it. The link is left UP throughout, because the cable case already worked.
test('reac_master_gone_is_re_decided',
  find_program('tests/master-gone-is-re-decided.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, fake_box_master],
  timeout : 240)

# --- whole-binary test: a refused open releases the engine it started (audit H1) ---------
#
# Audit 2026-09-24, H1: listener_open's fail_with_nodes exit dropped the nodes and nothing
# else, though a join had already opened and started the slave engine; hearing_serve then
# memset the listener under the running thread and retried into it. A box-master join
# whose reac-capture PipeWire refuses (tests/refuse_capture_shim.c, LD_PRELOAD) is refused
# five times, and the daemon's threads and fds must stay flat from refusal to refusal.
refuse_capture_shim = shared_module('refuse-capture-shim',
  ['tests/refuse_capture_shim.c'],
  dependencies : [pipewire_dep, cc.find_library('dl', required : false)],
  name_prefix : '',
  build_by_default : true)
test('reac_refused_open_releases_its_engine',
  find_program('tests/refused-open-releases-its-engine.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, fake_box_master, refuse_capture_shim],
  depends : [refuse_capture_shim],
  timeout : 180)

# --- whole-binary test: a segment with NO recognised box has NO node ---------
#
# Operator, 2026-09-16: "a segment with NO recognised box must not appear in the PipeWire
# graph at all." The desk's empty untagged trunk VLAN logged "the segment's door is on the
# graph now (reac-playback, no ports yet)" and the console rendered a device reading
# `none / 0 in`. An empty wire yields zero nodes, a box that enrols yields the sized pair,
# and a box that goes takes them away again -- the middle arm is the positive control for
# the other two. Needs libreac's fake_box (the same knob box-wakes-on-a-phy-edge uses);
# without it the test SKIPS rather than passing while proving nothing.
test('reac_no_box_no_node',
  find_program('tests/no-box-no-node.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, fake_box_path],
  timeout : 300)

# --- whole-binary test: the pair and the roster OUTLIVE A PIPEWIRE RESTART ---------------
#
# Desk 2026-09-23 13:44 (reac-pw-boot-2026-09-23): pipewire.service
# was restarted two seconds after an S-1608 enrolled on enp131s0. libpipewire leaves a
# stream whose server went away UNCONNECTED, with the node id it remembers, and the
# node-recover ladder read that as "on the graph" — twelve minutes of a box whose LED said
# enrolled, a roster row reading established, and no node for it anywhere. The same
# fixture as its siblings; the server is killed and restarted under the enrolled box, and
# the pair AND the roster node must come back as new ids, announced.
test('reac_graph_survives_a_pipewire_restart',
  find_program('tests/graph-survives-a-pipewire-restart.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, fake_box_path],
  timeout : 300)

# --- whole-binary test: the node COUNT follows the segment state (#108) ---------------
#
# The sibling above asks whether a pair is PRESENT; this one asks how many. A ghost is not
# an absence, it is a surplus -- `>= 2` is true of a real pair and true of a real pair
# beside a `box-model=none` twin on the same reac.segment, which is what the desk read on
# 2026-09-20 and what made a consumer's segment scan drop an established S-1608. So this
# asserts EXACTLY 2 while a box is recognised and EXACTLY 0 while not, across a box that
# arrives, leaves and arrives again, and across #107's capped-port refusal loop -- the two
# orderings the ghost was seen in. Needs libreac's fake_box; without it it SKIPS rather
# than passing while proving nothing.
test('reac_graph_state_follows_segment_state',
  find_program('tests/graph-state-follows-segment-state.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, fake_box_path],
  timeout : 420)

# --- whole-binary test: a box-role segment DECLARES ITS ROW on the wire ---------------
test('box-declares-its-row',
  find_program('tests/box-declares-its-row.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe],
  timeout : 240)

# --- source-shape test: `auto` never elects the box intent (2026-09-17 spec §7) ------
test('box-is-never-elected',
  find_program('tests/box-is-never-elected.sh'),
  timeout : 30)

# --- source-shape test: ONE segment, ONE word ----------------------------------------
#
# A `role = box` segment was spelled `BOX role`, `SLAVE role (…)` and `slave` in three
# lines of one log while the conf, the roster and the console all said `box`. Two of
# those lines need a mixer on a cable and the third a PipeWire failure, so what is read
# is the text the daemon is built to print. It also holds the reac-playback width to one
# spelling: the sizing failure used to report a number nobody had tried.
test('box-speaks-one-vocabulary',
  find_program('tests/box-speaks-one-vocabulary.sh'),
  timeout : 30)

# --- whole-binary test: a QUIET MIXER does not re-decide a `role = box` segment -------
#
# hearing_reevaluate's exemption was written for the pinned recorder and keyed on that
# intent alone, so a box on a wire whose desk is not switched on yet was torn down and
# re-heard every ten seconds. The control segment in the same run is a box master that
# joins and stops -- #97's own case, which MUST still be re-decided, so the absence
# asserted for the box segment is measured by a probe that has just seen a presence.
test('box-is-not-re-decided',
  find_program('tests/box-is-not-re-decided.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, fake_box_master],
  timeout : 300)

# --- whole-binary test: ONE node lists every segment, empty ones included -----
#
# The amendment before this one took the per-segment zero-port door off the graph and named
# its own cost: the console's segment roster is a GRAPH SCAN, so an empty segment has no row
# at all. The operator's other ruling the same day -- "not autodetecting is an error" -- is a
# sentence only a client that can SEE a probing segment can say. So the daemon publishes ONE
# port-less node, `reac-pw`, and every segment lives in its props. This drives the real binary
# on real netdevs: one empty wire and one with a box (the positive control for every reading),
# a drop-in in reac-pw.conf.d/ beating the hand-written file with its provenance on the row,
# and a box leaving -- which must move the PROPERTIES and not the node id. Needs libreac's
# fake_box; without it the test SKIPS rather than passing while proving nothing.
test('reac_roster_node_lists_every_segment',
  find_program('tests/roster-node-lists-every-segment.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe, fake_box_path],
  timeout : 300)

# --- whole-binary test: a roster group LEAVES with its segment (#106) -----------------
#
# The sibling above proves the roster is published and that a state change moves the
# PROPERTIES and not the node id. This one proves the other half, which was broken on the
# desk the whole time it was green: a segment that GOES must take its indexed keys with it.
# It reads the node's props off pw-dump -- the graph, never the daemon's own diff, which is
# what test_reac_roster.c already proves and what stayed right while the graph went wrong --
# and asserts reac.roster.n and the reac.roster.<i>.* groups AGREE after a drop. A PipeWire
# node cannot be told to drop a property (spec amendment 2026-09-20 sec b), so a shrink
# rebuilds the node; that rebuild is what this measures. No fake_box: the segment is taken
# away by deleting its netdev, which is the sharpest form of the carrier loss that produced
# the report.
test('reac_roster_keys_leave_with_their_segment',
  find_program('tests/roster-keys-leave-with-their-segment.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe],
  timeout : 300)

# --- whole-binary test: a PIN needs no door ----------------------------------
#
# The inverse of what this file used to assert, and that is the honest form of a
# superseded ruling: it was doors-open-before-the-wire.sh and it proved Q5 option C
# (2026-09-14), that a tap pinned on a silent wire and a pinned master with no box each
# published a door so a role could be set before anything enrolled. The requirement
# stands; reac-pw.conf answers it with no node at all, so the zero-port node is cost with
# no remaining benefit. Its positive control is a SECOND daemon with `--box` pinned --
# needed because a daemon serving only empty segments has no PipeWire client at all.
test('reac_pins_need_no_door',
  find_program('tests/pins-need-no-door.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe],
  timeout : 300)

# --- whole-binary test: a REFUSED head-amp write is VISIBLE to a second client -
# (2026-09-14-headamp-as-node-params §3a). A cell written to a segment with no
# preamps used to return nothing at all: set-param exited 0, the parse dropped the
# cell, and the caller had a successful write and no audio. On a private PipeWire,
# a pinned master with a silent wire: pw-cli writes, pw-dump reads back
# state=unavailable / refused=no-box, and asserted is still empty — the refused
# write entered no send table. The ACCEPTED path needs a box enrolled with us and
# is proven offline against libreac's golden (test_reac_headamp_state) and on the
# rig by a measured gain change.
test('reac_headamp_refusal_is_visible',
  find_program('tests/headamp-refusal-is-visible.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe],
  timeout : 180)

# --- the namespace tests are a SUITE, and the rpm runs that suite one at a time -------
#
# A ratchet, not a rule anyone has to remember: a test that mints a veth pair and a private
# PipeWire cannot share a machine with another doing the same, and %check's default
# one-process-per-core is what made the packaging check flake. Keyed on what a script does
# (it calls unshare), so a new one cannot forget the label.
test('netns_tests_are_serial',
  find_program('tests/netns-tests-are-serial.sh'),
  timeout : 30)

# --- ratchet: a namespace test's rc is a verdict, not an excuse -----------------
# Audit 2026-09-24, H3: seventeen whole-binary tests read ANY non-zero rc from their
# namespace body as SKIP, so a daemon that died at start was green. rc 77 is the only skip.
test('netns_rc_is_a_verdict',
  find_program('tests/netns-rc-is-a-verdict.sh'),
  timeout : 30)

# --- the protocol facts are reac-protocol's, byte for byte ---------------------------
# facts/reac_facts.h is a committed copy of reac-protocol's generated header. With a
# reac-protocol checkout reachable (option reac_protocol), this regenerates it with
# reac-protocol's own gen-facts.py and requires byte identity; without one it SKIPS
# and the build uses the committed copy -- libreac's facts-drift-check, one mechanism.
reac_protocol_dir = get_option('reac_protocol')
if not reac_protocol_dir.startswith('/')
  reac_protocol_dir = meson.project_source_root() / reac_protocol_dir
endif
test('facts_drift',
  find_program('python3'),
  args : [files('tools/facts.py'), 'drift', reac_protocol_dir],
  timeout : 60)

# --- whole-binary test: the PACKAGED (no-argument) shape actually starts ------
#
# auto-spine §5 gives the unit no flags, and since the trunk-VLAN spec's amendment
# (2026-09-02 §d) nothing is configured for it either — REAC_IFACES retired with the
# per-interface files, so the daemon HEARS its segments. An `argc < 2` guard answered
# usage before any of that was reached, and the packaged service could never start
# (measured on the rig, 2026-08-31). Needs no NIC: it asserts help still works with
# nothing configured, and that a bare `reac-pw` gets PAST argument handling.
test('reac_packaged_shape',
  find_program('tests/packaged-shape-starts.sh'),
  args : [reac_pw_exe,
          meson.current_source_dir() / 'packaging' / 'reac-pw.service',
          meson.current_source_dir() / 'packaging' / '90-reac-pw.preset'],
  timeout : 30)

# --- whole-binary test: the daemon refuses to start as uid 0 (2026-09-18) -----
# A root instance runs under a DIFFERENT systemd --user manager than the console
# session's and binds the same abstract segment-lock socket — measured to win it
# and lock the console user's own daemon out. `unshare -r` maps us to a fake root
# inside a private user namespace; a positive control proves the normal uid is
# NOT refused, before trusting that the fake-root run is.
test('reac_refuses_root',
  find_program('tests/refuses-root.sh'),
  suite : 'netns',
  is_parallel : false,
  args : [reac_pw_exe],
  timeout : 30)

# The %posttrans auto-enable's DECISION (#104): exactly one real interactive user, or
# nobody. loginctl/systemctl are mocked, so it needs no namespace, no daemon and no rpm --
# it runs in the parallel pass. The tables are the real shape measured on a desk.
test('reac_safe_enable_selects_one_user',
  find_program('tests/safe-enable-selects-one-user.sh'),
  args : [meson.current_source_dir() / 'packaging' / 'reac-pw-safe-enable.sh'],
  timeout : 30)

# The NetworkManager drop-in is the union of reac-pw's `reac-pw:` interface aliases and of
# nothing else. sysfs, /proc/sys, nmcli and ip are mocked: no NetworkManager, no root.
test('reac_nm_unmanaged_follows_the_marks',
  find_program('tests/nm-unmanaged-follows-the-marks.sh'),
  args : [meson.current_source_dir() / 'packaging' / 'reac-pw-nm-unmanaged.sh'],
  timeout : 30)

# --- unit test: re-deciding a segment that is already UP (0.5.4) --------------
# The yield, the retake and the door's end, as one pure table. It lived inline in
# main.c, where the only thing that could exercise it was the 70 s veth run -- which
# cannot choose which route took the wire, so the case this release is about was
# untestable by construction.
watch_test = executable('test_reac_watch',
  ['tests/test_reac_watch.c', 'src/reac_watch.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep])
test('reac_watch', watch_test)

# --- unit test: the ProcessLatency smoother (pure EMA + hysteresis) -----------
# No PipeWire, no socket: feeds a 38..113 depth sawtooth through the EMA and
# asserts the smoothed latency settles into a tight band and stops re-advertising.
lat_test = executable('test_reac_lat',
  ['tests/test_reac_lat.c', 'src/reac_lat.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep, m_dep])
test('reac_lat', lat_test)

# --- unit test: the SCHED_FIFO cadence pacer (ring + interval) -----------------
# No socket, no RT privilege needed: tests the frame ring SPSC behaviour, the
# per-rate slot period (125/250/272 us), and the master-stamp-on-egress path.
pacer_test = executable('test_reac_pacer',
  ['tests/test_reac_pacer.c', 'src/reac_knock_credit.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep, threads_dep, m_dep])
test('reac_pacer', pacer_test)

# --- unit test: the output-gain staging kernel (pure ramp, no PipeWire) --------
# Exercises reac_gain_ramp_block directly: unity is a no-op, a target is reached
# within the expected sample count, mute ramps to zero, and two channels ramp
# independently. No socket/PipeWire — the pure kernel the RT sink path calls.
gain_test = executable('test_reac_gain',
  ['tests/test_reac_gain.c', 'src/reac_gain.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep, m_dep])
test('reac_gain', gain_test)

# --- source gate: every dequeued pw_buffer is queued back, on every path -------
# Not a unit test: the defect lives in a PipeWire callback no unit test reaches,
# and it fails in the way that hides best — the sink re-sends one stale frame
# forever at exactly the right rate with a valid non-zero audio region, so every
# check except "does this frame differ from the last one" reports health.
python3 = find_program('python3', required : true)
test('sink_buffer_pairing', python3,
  args : [meson.current_source_dir() / 'tests' / 'test_sink_buffer_pairing.py'])

# --- unit test: a box no row names is sized and named from its declaration -------
# 2026-10-06: a stock S-4000S re-fitted to 16 in / 24 out declared 02 x4, 01 x6, 03 x2
# and got no nodes, because they were built only for a matched row.
box_row_test = executable('test_reac_box_row',
  ['tests/test_reac_box_row.c', 'src/reac_box_row.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep])
test('reac_box_row', box_row_test)

# --- unit test: a box no row names asks for its identity and publishes it ------
# 2026-10-06: the S-4000S-4000 (40 in / 0 out) has only a reac-capture, and only
# reac-playback was stamped with the identity page, so /reac/segment read no firmware,
# no hw block and no patch name for it. Granted at 40, its sweep carries the identity
# poll, and its replies reach the badge reac-capture is stamped with.
box_capture_identity_test = executable('test_reac_box_capture_identity',
  ['tests/test_reac_box_capture_identity.c', 'src/reac_box_row.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep, threads_dep, m_dep])
test('reac_box_capture_identity', box_capture_identity_test)

# --- unit test: the boolean-knob reader + the clock-discipline default ---------
# One reading for every boolean knob (REACPW_CLOCK_FOLLOW=0 used to mean ON, because
# the reader only asked whether the variable was set), and the shipped default of the
# clock discipline: FOLLOW (arbitration spec S3). Pure; no socket, no PipeWire.
envflag_test = executable('test_reac_envflag',
  ['tests/test_reac_envflag.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep])
test('reac_envflag', envflag_test)

# --- unit test: every env/conf knob is DISCOVERED and PUBLISHED (2026-09-17) --------
# The table (src/reac_knobs.c) is well-formed and every non-conf_capable entry names
# why; reac_conf_flag is layered with the right default on a miss; reac_knobs_announce
# prints the exact grammar for a knob that is set and NOTHING for one that is not; and
# the code<->doc round trip against docs/ENV-KNOBS.md holds both directions, so the two
# cannot drift the way the file's own closing promise never checked.
knob_table_test = executable('test_reac_knob_table',
  ['tests/test_reac_knob_table.c', 'src/reac_knobs.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep])
test('reac_knob_table', knob_table_test,
  env : ['REACPW_SRCDIR=' + meson.current_source_dir()])

# --- unit test: the bounded rebuild ladder for a node that never reached the graph ---
# Pure decision (reac_node_recover): a grace before acting, a doubling window, a bound,
# one terminal line, and a reset the moment the node appears. The defect it answers cost
# nine minutes of dead input patches with a journal that read like success.
recover_test = executable('test_reac_node_recover',
  ['tests/test_reac_node_recover.c', 'src/reac_node_recover.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep, m_dep])
test('reac_node_recover', recover_test)

# --- unit test: the LINK BUDGET, measured on the desk 2026-09-16 ---------------
# enp131s0 is a 100 Mbit/s port and carried FOUR declared 96 kHz master segments (the
# untagged one plus VLANs 11/12/13): 387 Mbit/s offered onto a 100 Mbit/s wire, the etf
# qdisc discarding 23 784 pkt/s as overlimit — 75% of every segment's frames, evenly —
# and an S-1608 in slave mode sitting at rx=0 because it never saw a master stream it
# could lock to. A master that does not fit REFUSES; delivering a quarter of a stream
# with every health sign green is the silent clamp this repo exists to refuse. Pure.
link_budget_test = executable('test_reac_link_budget',
  ['tests/test_reac_link_budget.c', 'src/reac_link_budget.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep])
test('reac_link_budget', link_budget_test)

# --- unit test: the WAKE LADDER, and when the master may break its own link ---------
# 2026-09-16: an S-1608 that dropped while the desk was suspended ignored ~1620 completed
# scene pushes over 73 minutes and two processes, on a wire carrying 8003 of our frames a
# second. Its firmware leaves that state on a PHY edge and on nothing else. This pins both
# halves: the replayed live window must produce the edges the ladder allows, and each of
# the facts that forbid an edge — no carrier, an unreadable carrier, a box already talking,
# a push of ours that never completed, a sibling segment being served, a spent ladder —
# must refuse it by name, each with a control showing the same call succeeding. Pure.
wake_test = executable('test_reac_wake',
  ['tests/test_reac_wake.c', 'src/reac_wake.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep])
test('reac_wake', wake_test)

# --- BINDING test: the enrolment decisions this daemon links against -------------
# THE TWO STATE MACHINES MOVED TO libreac 1.4.0 (its own tests/test_reac_knock.c and
# tests/test_reac_tapwait.c assert the arithmetic, where a libreac change can see them go
# red). What is left here is the only question reac-pw can answer and libreac cannot: does
# the library THIS BINARY IS LINKED AGAINST still carry the two rules main.c's poll depends
# on? It calls the installed surface -- no reac-pw source, no local header -- and asserts
# the pair the desk paid nine minutes for on 2026-09-21:
#
#   a cancelled masterless observation RE-OPENS, measured from the last frame heard
#   an unplaced sighting stops binding the hunt once it is stale
#
# A floor bump that silently picked up a library without them, or a libreac that reverted
# either latch, fails HERE, at reac-pw's own gate, instead of on a desk. Pure; no socket.
# --- unit test: THE WIRE MODEL, a silent wire is knocked on and never flooded (#40) ----
# A fake wire counts every frame by kind; on it, nothing, fake_box, a box that answers only
# through FILLER, or a trunk parent. Ten silent minutes carry no stream frame and not one
# FILLER, at the backoff's gaps, against budgets derived from libreac's own master; the first
# answer ends the knock and starts the stream, whose master reaches ENROLL; a trunk parent
# goes quiet on its VLANs' tags; the knock's wake ladder bounces on schedule. Handing the
# licence back to the hunt (the old probe) turns most of the eleven arms red.
knock_wire_test = executable('test_reac_knock_wire',
  ['tests/test_reac_knock_wire.c', 'src/reac_knock_wire.c', 'src/reac_wake.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep])
test('reac_knock_wire', knock_wire_test)

# --- unit test: a MASTER THAT STREAMS AT NOBODY STOPS (#40) ---------------------------------
# The master a knock's answer started stops on the poll that shows no box: 2 s with not one
# box frame while it courts, at once when libreac gives an enrolled box up. With values.
empty_master_test = executable('test_reac_empty_master',
  ['tests/test_reac_empty_master.c', 'src/reac_empty_master.c'],
  include_directories : src_inc)
test('reac_empty_master', empty_master_test)

# --- unit test: --box MODEL[:LABEL], the fixed-installation pin ---------------
# Restored 2026-08-22 (operator: reac-pw is a daemon, a permanent rig pins its patch;
# a console autodetects — complementary, not alternatives). The pin declares NODE
# GEOMETRY AND LABEL only: --box was retired in 902bf39 because it pre-fabricated a
# head-amp enrollment from a guess, and a box granted slots it does not own links,
# streams audio and silently ignores every head-amp record.
box_pin_test = executable('test_reac_box_pin',
  ['tests/test_reac_box_pin.c', 'src/reac_box_pin.c',
  ],
  dependencies : [libreac_dep, libreac_transport_dep],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep, m_dep])
test('reac_box_pin', box_pin_test)

# --- unit test: parse LIVE head-amp control out of a SPA_PARAM_Props pod (#203) -
# Pure SPA-pod parse (no PipeWire, no graph): builds Props objects carrying
# SPA_PROP_params keyed "reac.headamp.<ch>.<param>" and asserts the decode, the
# per-param range gate, the Bool/Int/Float encodings, and (key,value) resync.
headamp_prop_test = executable('test_reac_headamp_prop',
  ['tests/test_reac_headamp_prop.c', 'src/reac_headamp_prop.c',
   'src/reac_headamp_state.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep, spa_dep])
test('reac_headamp_prop', headamp_prop_test)

# --- unit test: the head-amp door's READ side (2026-09-14-headamp-as-node- -----
# params.md §3a) — reac.headamp.asserted / .state / .refused / .sens.max.
# Two halves, both offline. (1) THE WIRE: a Props pod carrying
# reac.headamp.32.phantom = 1 driven through reac_headamp_prop_parse ->
# reac_pacer_headamp_set -> the pacer drain -> reac_headamp_tx_next ->
# reac_ctrl_stamp_headamp, and the emitted frame's control block byte-compared to
# libreac's captured golden (FX_L4_HEADAMP, an M-200 commanding an S-1608). The
# golden bytes are the oracle: a check that the parse returned what the builder
# built would prove nothing. (2) THE ANSWER: each refusal code produced and read
# back, the capability decision, and the asserted cell list a second client
# renders from.
headamp_state_test = executable('test_reac_headamp_state',
  ['tests/test_reac_headamp_state.c', 'src/reac_headamp_prop.c',
   'src/reac_headamp_state.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep, threads_dep, m_dep, spa_dep])
test('reac_headamp_state', headamp_state_test)

# --- unit test: reac.cfg.rate — closed list, drivability, decision core, and --
# the internal re-establish (2026-08-26-reac-runtime-config) ---------------
# Pure decision core (reac_rate_cfg.c: closed-list membership, the drivable-mask
# arithmetic, the accept/refuse decision, the SPA_PROP_params parse) plus the
# pacer-level re-establish (reac_pacer_apply_rate/_request_rate/_rate_drain),
# driven directly with no socket/RT privilege — same style as
# test_reac_headamp_live. Proves: an in-list, drivable rate re-establishes with
# the grant/dwell sequence byte-identical in SHAPE at the new cadence; an
# out-of-list rate is refused and nothing moves; a slave refuses every
# assertion; and the default is the best rate a (possibly narrowed) drivable
# mask allows — the segment-cannot-do-96k case included, since no real
# drivability probe exists yet to supply that mask from the wire (see
# reac_rate_cfg.h's honesty clause).
rate_cfg_test = executable('test_reac_rate_cfg',
  ['tests/test_reac_rate_cfg.c', 'src/reac_rate_cfg.c', ],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep, threads_dep, m_dep, spa_dep])
test('reac_rate_cfg', rate_cfg_test)

# --- unit test: reac.cfg.role — the ROLE half of runtime config ---------------
# (2026-08-26-reac-runtime-config). The DECISION core only: the parse, the
# same-role-is-a-no-op detection, and the state a request publishes when nothing
# behind it is carrying the change out. The swap itself and its lifecycle are
# reac_role_swap's (next block) — this module keeps the pure decision and knows
# nothing about engines. Proves: role 0/1 parse (Int and the Float-bridge
# tolerance), a malformed value refuses, a same-role assertion is an idempotent
# "applied" no-op, and a role-changing assertion answers
# "role_reestablish_pending" — sabotage-verified by flipping the state string
# comparison in reac_role_cfg_apply_state to prove the shape-identity test
# actually distinguishes the two answers.
role_cfg_test = executable('test_reac_role_cfg',
  ['tests/test_reac_role_cfg.c', 'src/reac_role_cfg.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep, spa_dep])
test('reac_role_cfg', role_cfg_test)

# --- unit test: reac_sink_format — the reac-playback node's Format pod and ---
# the renegotiate decision (2026-08-26-clock-tabs-and-reac-pace-coupling
# §4.3 increment 3). Pure: no pw_stream, no socket. Proves the format-build
# function returns the asserted rate (parsed back with spa_format_audio_raw_
# parse, not just re-read from the builder call's own argument) at the boot
# rate and again after a change, that the channel count clamps into
# [0, REAC_MAX_CHANNELS], and that reac_sink_format_needs_update fires exactly
# when the pacer's rate has moved past the node's last-pushed rate — never on
# a byte-identical boot, never on a refused write (which leaves the pacer's
# rate_hz untouched), never on a non-positive rate. Sabotage-verified: an
# inverted needs_update would make a real rate change look like a no-op AND
# an unrelated poll look like a pending renegotiation — this test drives both
# arms so flipping the comparison turns a currently-green line red.
sink_format_test = executable('test_reac_sink_format',
  ['tests/test_reac_sink_format.c', 'src/reac_sink_format.c'],
  include_directories : src_inc,
  dependencies : [spa_dep, libreac_dep, libreac_transport_dep])
test('reac_sink_format', sink_format_test)

# --- unit test: reac_source_node's rate renegotiation (one wire, one rate) ---
# (2026-08-26-clock-tabs-and-reac-pace-coupling §1b: "capture AND playback
# follow it together"). reac_source_node.c's source_reconnect_rate/reac_
# source_node_publish_rate are not pure (they own a live pw_stream, like the
# sink's own reconnect) and are not exercised here; what IS pure and is
# exactly what those functions call is reac_sink_format.h's shared builder +
# decisions, now proven at the source's own call shape: the pod carries the
# new rate at real REAC box widths (8/16/32/40), the reconnect gate fires
# exactly on an accepted change and stays false on a matching or refused
# rate, and the post-attempt rate matches the connect outcome. Sabotage-
# verified: an inverted gate would silently reintroduce the exact bug this
# task fixes (capture stuck presenting the boot rate).
source_format_test = executable('test_reac_source_format',
  ['tests/test_reac_source_format.c', 'src/reac_sink_format.c'],
  include_directories : src_inc,
  dependencies : [spa_dep, libreac_dep, libreac_transport_dep])
test('reac_source_format', source_format_test)

# --- unit test: reac_node_ensure — the shared same-box-or-rebuild decision ---
# (2026-08-26-duplicate-reac-node). Pure: no pw_stream, no
# socket. Proves the identity comparison both reac_sink_node_ensure and
# reac_source_node_ensure now share: no existing node always rebuilds, an
# identical width+label is a no-op, a width change rebuilds, and — the bug
# this closes — a same-width LABEL change rebuilds too (reac_source_node_
# ensure used to compare width only, so a re-enrolled same-width box or a
# relabelled pin left reac-capture's node.description/badges stamped with the
# old box's identity forever). Sabotage-verified: the width-only comparison
# reac_source_node_ensure used to make is proven to disagree with the real
# function on the label-only-change case.
node_ensure_test = executable('test_reac_node_ensure',
  ['tests/test_reac_node_ensure.c', 'src/reac_node_ensure.c'],
  include_directories : src_inc,
  dependencies : [libreac_dep, libreac_transport_dep, m_dep])
test('reac_node_ensure', node_ensure_test)

# --- benchmark (not a test): per-frame CPU cost of the two hot paths ----------
# Run by hand — it is a measurement, not a gate. It `#include`s reac_rx.c (now in
# libreac-transport) to reach the static feed_frame, which is the only way to
# benchmark the SHIPPING decode path instead of a copy of it that would drift.
# reac_rx.c's SOURCE is not part of the installed -devel package (only the
# header is), so this needs a sibling libreac checkout's transport/src — point
# -Dlibreac_transport_srcdir at it (declared near the top, also used by the
# rt_single_door test's positive control). Not build_by_default any more: a
# normal checkout without that sibling tree would otherwise fail `ninja`
# outright for a target nothing else here depends on.
bench_hotpath_inc = [src_inc, include_directories('tests')]
if libreac_transport_srcdir != ''
  bench_hotpath_inc += include_directories(libreac_transport_srcdir, is_system : true)
endif
bench_hotpath = executable('bench_hotpath',
  ['tools/bench_hotpath.c'],
  include_directories : bench_hotpath_inc,
  dependencies : [libreac_dep, libreac_transport_dep, threads_dep, m_dep],
  build_by_default : false)

# --- CLI text: --headamp's advertised CH bound must match the head-amp space ---
# (REAC_HEADAMP_MAX_CH, 48 slots — an S-1608 at base 0x20 occupies 32..47) rather
# than the audio-fabric width (40, wire channels 0..39). Runs the built binary
# with no args (usage()) and reads its own stderr, so a future edit of the help
# string is caught the same way an operator misreading it would be.
usage_test = executable('test_reac_headamp_usage',
  ['tests/test_reac_headamp_usage.c'])
test('reac_headamp_usage', usage_test, args : [reac_pw_exe.full_path()])

# --- meson-text: every whole-binary header sits above ITS OWN test -----------
# Five `# --- ... ---` headers had drifted onto the wrong test (a later block
# wedged in between a header and the test it was written for, and nothing
# checked) — line ~430 above the wrong test, ~573, ~822, ~1062, ~1194, all
# fixed 2026-09-17. Pure text: walks every header, finds the next `test(` call
# before another header intervenes, and refuses when the header shares no
# normalized keyword with that test's `tests/*.sh` script name. Sabotage-
# verified by swapping two headers in a scratch copy and requiring both
# directions to fail.
test('meson_headers_sit_above_their_test', python3,
  args : [meson.current_source_dir() / 'tests' / 'meson-headers-sit-above-their-test.py',
          meson.current_source_dir() / 'meson.build'])

# --- text: every declared reac_code is emitted, and bare refusals stay under a floor --
# "We need the error codes and not only messages" (2026-09-17). Every token of the daemon's
# share of libreac's <reac/reac_code.h> list (tests/reac-code-daemon-tokens.txt) must be
# reachable through reac_code_emit somewhere in src/*.c, and no copy of what libreac declares
# or does may appear in src/ (ONE HOME, 1.0.42), and a REFUSED/FATAL/failed-shaped bare fprintf that still bypasses the helper
# is counted against a floor that may only fall (proportionate migration, spec §2) — a
# change that adds a new one, or un-migrates one already counted, fails the build.
test('reac_code_conformance', python3,
  args : [meson.current_source_dir() / 'tests' / 'reac-code-conformance.py',
          meson.current_source_dir() / 'src',
          meson.current_source_dir() / 'tests' / 'reac-code-daemon-tokens.txt',
          libreac_dep.get_variable(pkgconfig : 'includedir')])

# --- text: every tests/*.sh grep/wait_for pattern maps to a producer, or is debt ------
# "Let's be precise, let's organise tests to not depend on greps" (2026-09-17). Matches
# every quoted grep/wait_for pattern in tests/*.sh against src/*.c's printf-family format
# strings (conversions wildcarded) and <reac/reac_code.h>'s tokens; refuses a pattern with
# neither a producer nor a named line in tests/known-debt-grep-patterns.txt, which is
# itself a ratchet (may only shrink). Sabotage-verified by injecting an unmapped pattern
# into a scratch copy of tests/ and requiring it to be refused.
test('reac_asserted_lines_have_a_producer', python3,
  args : [meson.current_source_dir() / 'tests' / 'asserted-lines-have-a-producer.py',
          meson.current_source_dir() / 'tests',
          meson.current_source_dir() / 'src',
          meson.current_source_dir() / 'tests' / 'known-debt-grep-patterns.txt',
          libreac_dep.get_variable(pkgconfig : 'includedir')])

# --- text: the public tree cites internal documents by slug; the README builds nothing --
# Specs, notes, audits and rig files live in the private ops repository. A citation in
# code, tests, packaging or the user docs is the bare slug, never a docs path or a dated
# `.md` filename that resolves to nothing in a public checkout; README.md carries no build
# command and BUILDING.md carries the build. Sabotage-verified by putting a spec path back
# into a src/ comment and requiring it to be refused by file and line.
test('reac_public_tree_cites_slugs', python3,
  args : [meson.current_source_dir() / 'tests' / 'public-tree-cites-slugs.py',
          meson.current_source_dir()])

# --- text: main.c locks the slave side's receive gate (src/reac_rx_bind.c) ----------------
# The unit test proves the lock; this proves the daemon asks for it, before the feeder starts.
test('reac_rx_bind_is_wired', python3,
  args : [meson.current_source_dir() / 'tests' / 'rx-bind-is-wired.py',
          meson.current_source_dir()])

# --- text: the public tree carries no compiled Python and every source has its header -----
# A tracked __pycache__/*.pyc embeds the build machine's paths, and a *.c *.h *.py *.sh with no
# SPDX-License-Identifier + Copyright pair states no licence. tools/check-public-hygiene.sh
# reads the git index and refuses both; it SKIPs (77) where there is no git checkout.
test('public_hygiene', find_program('bash'),
  args : [meson.current_source_dir() / 'tools' / 'check-public-hygiene.sh',
          meson.current_source_dir()])
