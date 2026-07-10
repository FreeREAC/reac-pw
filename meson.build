# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# reac-pw — PipeWire-native REAC endpoint (RX source node; TX sink scaffold).
#
# Reuses, does not reinvent:
#   - libreac           RX validate / counter / rate-detect      (subproject wrap)
#   - reac-aes67 core   plain-LE decode + AF_PACKET + pcap reader (sibling source)
# and links against the system libpipewire-0.3 / libspa-0.2 (Fedora pipewire-devel).

project('reac-pw', 'c',
  version : '0.1.0',
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

# --- libreac: RX/measure core, pulled as a meson subproject -----------------
# subprojects/libreac.wrap clones FreeREAC/libreac. It builds a static lib and
# exposes `libreac_dep` (see subprojects/packagefiles/libreac/meson.build, which
# the wrap drops in via patch_directory since upstream libreac ships only a
# hand Makefile). The header is <reac/reac.h>.
# Prefer the system libreac (Fedora libreac-devel -> found via pkg-config and
# linked dynamically). Fall back to the bundled subproject where there is no
# system lib (OpenWrt SDK / a plain dev checkout).
libreac_dep = dependency('libreac',
  fallback : ['libreac', 'libreac_dep'],
  required : true)

# --- reac-aes67 decode/capture/pcap core: reuse the sibling checkout ---------
# These three files are dependency-free and already proven on-rig (plain-LE
# sample-major decode, coherence 0.999). We compile them straight in rather than
# fork them, exactly as reac-aes67's own Makefile compiles libreac's reac.c in.
# Override with -Dreac_aes67=/path/to/reac-aes67 if the checkout lives elsewhere.
reac_aes67 = get_option('reac_aes67')
reac_aes67_src = [
  reac_aes67 / 'src' / 'reac_decode.c',
  reac_aes67 / 'src' / 'reac_capture.c',
  reac_aes67 / 'src' / 'pcap_source.c',
]
foreach f : reac_aes67_src
  if not import('fs').is_file(f)
    error('reac-aes67 core not found at @0@ — clone github.com/FreeREAC/reac-aes67 as a sibling, or pass -Dreac_aes67=PATH'.format(reac_aes67))
  endif
endforeach
reac_aes67_inc = include_directories(reac_aes67 / 'src')

# --- the reac-pw client -----------------------------------------------------
reac_pw_src = [
  'src/main.c',
  'src/reac_ring.c',
  'src/reac_rx.c',
  'src/reac_tx.c',
  'src/reac_ctrl.c',
  'src/reac_fsm.c',
  'src/reac_master.c',
  'src/reac_pacer.c',
  'src/reac_slave.c',
  'src/reac_source_node.c',
  'src/reac_sink_node.c',
]

executable('reac-pw',
  reac_pw_src + reac_aes67_src,
  include_directories : [reac_aes67_inc],
  dependencies : [pipewire_dep, spa_dep, threads_dep, m_dep, libreac_dep],
  install : true)

# --- unit test: the lock-free ring (RT-safe SPSC), no PipeWire needed --------
ring_test = executable('test_reac_ring',
  ['tests/test_reac_ring.c', 'src/reac_ring.c'],
  dependencies : [threads_dep])
test('reac_ring', ring_test)

# --- unit test: TX encoder round-trips through the reac-aes67 decode core -----
# No PipeWire, no socket — pure encode->decode bit-compare. Compiles reac_tx.c +
# reac_decode.c with both include trees (<reac/reac.h> from libreac_dep, the
# reac_decode/reac_mode headers from reac_aes67_inc).
tx_test = executable('test_reac_tx',
  ['tests/test_reac_tx.c', 'src/reac_tx.c', reac_aes67 / 'src' / 'reac_decode.c'],
  include_directories : [reac_aes67_inc, include_directories('src')],
  dependencies : [libreac_dep, m_dep])
test('reac_tx', tx_test)

# --- unit test: control-plane builders/parser/checksum (ground-truthed) -------
ctrl_test = executable('test_reac_ctrl',
  ['tests/test_reac_ctrl.c', 'src/reac_ctrl.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, m_dep])
test('reac_ctrl', ctrl_test)

# --- unit test: the JOIN/HOLD connection FSM (pure state machine) -------------
fsm_test = executable('test_reac_fsm',
  ['tests/test_reac_fsm.c', 'src/reac_fsm.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep])
test('reac_fsm', fsm_test)

# --- unit test: the MASTER-role handshake (cdea/cfea bytes vs the captures) ----
# Asserts the established channel-map / cfea announce / probe / grant blocks the
# master stamps match the captured M-5000 bytes + satisfy the checksum, the chan-
# map cursor walks the full 40-ch map, and the FSM sequences IDLE->...->ESTABLISHED.
master_test = executable('test_reac_master',
  ['tests/test_reac_master.c', 'src/reac_master.c', 'src/reac_ctrl.c',
   'src/reac_tx.c', reac_aes67 / 'src' / 'reac_decode.c'],
  include_directories : [reac_aes67_inc, include_directories('src')],
  dependencies : [libreac_dep, m_dep])
test('reac_master', master_test)

# --- unit test: the SCHED_FIFO cadence pacer (ring + interval) -----------------
# No socket, no RT privilege needed: tests the frame ring SPSC behaviour, the
# per-rate slot period (125/250/272 us), and the master-stamp-on-egress path.
pacer_test = executable('test_reac_pacer',
  ['tests/test_reac_pacer.c', 'src/reac_pacer.c', 'src/reac_master.c',
   'src/reac_ctrl.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, threads_dep, m_dep])
test('reac_pacer', pacer_test)

# --- unit test: the SLAVE-role establishment decision core (pure, no socket) ----
# Drives the slave decision core (reac_slave_step_* over the reac_fsm brain) through
# the gold §13d slave sequence: PHY-up -> flood presence -> RX the master's
# probe/grant -> TX-mute dwell -> ESTABLISHED unicast upstream + heartbeat -> HOLD
# re-arm on master heartbeat -> peer-gone drop. The master frames are synthesized
# the way reac_ctrl_parse would yield them from a real master broadcast (the same
# byte layout as the captured M-5000 in reac-captures), so the establishment is
# exercised correct-by-construction against the capture-derived control kinds.
slave_test = executable('test_reac_slave',
  ['tests/test_reac_slave.c', 'src/reac_slave.c', 'src/reac_fsm.c',
   'src/reac_ctrl.c', 'src/reac_ring.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, threads_dep, m_dep])
test('reac_slave', slave_test)

# --- unit test: --role selection parsing (master default, slave needs --tx) -----
# Compiles a tiny harness that exercises the same role-parse + validation logic
# main.c uses, with no PipeWire / socket. Asserts: default is master; --role slave
# is parsed; --role slave without --tx is rejected; an unknown role is rejected.
role_test = executable('test_reac_role',
  ['tests/test_reac_role.c'],
  include_directories : [include_directories('src')])
test('reac_role', role_test)
