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
# >=0.4.0: the braid/sample layout oracles, the upstream decode, the OHRCA
# clean-len rule AND (since 0.4.0) the ENCODE side — reac_braid_encode +
# reac_downstream_build — live there now. This version floor is load-bearing:
# reac-pw no longer carries its own encoder, so a 0.3.x libreac must fail at
# configure time, not at link time.
libreac_dep = dependency('libreac', version : '>=0.4.0',
  fallback : ['libreac', 'libreac_dep'],
  required : true)

# The REAC decode / capture / pcap core comes from libreac via
# <reac/reac_decode.h> etc., and since 0.3.0 so do the braid codec
# (<reac/reac_braid.h>), the f32<->s24 pair (<reac/reac_sample.h>) and the
# box-upstream decode (<reac/reac_upstream.h>). Since 0.4.0 the ENCODE side is
# there too (<reac/reac_encode.h>): reac_downstream_build builds the 1492 B
# master broadcast and reac_braid_encode lays the braided audio region of the
# box->master frames reac_ctrl builds. reac-pw keeps only what libreac will not
# do — AF_PACKET emission (reac_tx.c), the SCHED_FIFO pacer, and the
# control-block/checksum handshake. No local copies of any layout.

# --- the reac-pw client -----------------------------------------------------
reac_pw_src = [
  'src/main.c',
  'src/reac_ring.c',
  'src/reac_rx.c',
  'src/reac_tx.c',
  'src/reac_ctrl.c',
  'src/reac_fsm.c',
  'src/reac_grant.c',
  'src/reac_master.c',
  'src/reac_master_fsm.c',
  'src/reac_pacer.c',
  'src/reac_slave.c',
  'src/reac_mac.c',
  'src/reac_headamp_tx.c',
  'src/reac_headamp_prop.c',
  'src/reac_source_node.c',
  'src/reac_sink_node.c',
  'src/reac_lat.c',
  'src/reac_gain.c',
  'src/reac_boxreg.c',
  'src/reac_link_state.c',
  'src/reac_disco.c',
  'src/reac_clock.c',
]

executable('reac-pw',
  reac_pw_src,
  dependencies : [pipewire_dep, spa_dep, threads_dep, m_dep, libreac_dep],
  install : true)

# --- unit test: the lock-free ring (RT-safe SPSC), no PipeWire needed --------
ring_test = executable('test_reac_ring',
  ['tests/test_reac_ring.c', 'src/reac_ring.c'],
  dependencies : [threads_dep])
test('reac_ring', ring_test)

# --- unit test: TX encoder round-trips through the libreac decode core ---------
# No PipeWire, no socket — pure encode->decode bit-compare. The encoder itself is
# libreac's reac_downstream_build now; src/reac_tx.c is still linked for
# reac_eth_crc32 (the OHRCA-trailer RE verifier, not part of any encode path).
tx_test = executable('test_reac_tx',
  ['tests/test_reac_tx.c', 'src/reac_tx.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, m_dep])
test('reac_tx', tx_test)

# --- BYTE-IDENTITY gate for the encoder's move into libreac -------------------
# main auto-deploys to the live rig, so this pins the emitted BYTES, not just the
# behaviour: FNV-1a-64 digests of a deterministic corpus (1120 downstream frames,
# 9240 box->master frames across every builder and box width), whose expected
# constants were computed by running the PRE-MOVE encoder. libreac's own
# tests/test_encode.c pins the same downstream constant from the other side of
# the wrap. A mismatch is a wire-format regression — never update the constant.
encode_golden_test = executable('test_reac_encode_golden',
  ['tests/test_reac_encode_golden.c', 'src/reac_ctrl.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, m_dep])
test('reac_encode_golden', encode_golden_test)

# --- unit test: the UPSTREAM (stagebox->master) frame decode -------------------
# Captured-frame fixtures (MAC-sanitized) + the resolved braid layout; no I/O.
upstream_test = executable('test_reac_upstream',
  ['tests/test_reac_upstream.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep])
test('reac_upstream', upstream_test)

# --- unit test: the RX stream gate (downstream vs upstream accept) ------------
# Runs the real feeder thread over a temp pcap that interleaves both streams
# (synthetic 40-ch broadcast + the captured box return + a second box).
rx_gate_test = executable('test_reac_rx_gate',
  ['tests/test_reac_rx_gate.c', 'src/reac_rx.c', 'src/reac_ring.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, threads_dep, m_dep])
test('reac_rx_gate', rx_gate_test)

# --- unit test: the OHRCA duplicate-frame guard (frames_dup) ------------------
# Runs the real feeder thread over a temp pcap of DOUBLED byte-identical
# delivery (the mirrored-RX-path / doubled-cadence granulated-audio bug):
# exactly one twin dropped per frame, single-cadence ring, and a negative —
# distinct frames at doubled cadence pass untouched (dup == 0).
rx_dup_test = executable('test_reac_rx_dup',
  ['tests/test_reac_rx_dup.c', 'src/reac_rx.c', 'src/reac_ring.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, threads_dep, m_dep])
test('reac_rx_dup', rx_dup_test)

# --- unit test: control-plane builders/parser/checksum (ground-truthed) -------
ctrl_test = executable('test_reac_ctrl',
  ['tests/test_reac_ctrl.c', 'src/reac_ctrl.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, m_dep])
test('reac_ctrl', ctrl_test)

# --- unit test: head-amp source control (op 04 03 TAG 01 01, M-200-truthed) ---
headamp_test = executable('test_reac_headamp',
  ['tests/test_reac_headamp.c', 'src/reac_ctrl.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, m_dep])
test('reac_headamp', headamp_test)

# --- unit test: the multi-box registry (fabric slot allocation + names) -------
boxreg_test = executable('test_reac_boxreg',
  ['tests/test_reac_boxreg.c', 'src/reac_boxreg.c'],
  include_directories : [include_directories('src')])
test('reac_boxreg', boxreg_test)

# --- unit test: the JOIN/HOLD connection FSM (pure state machine) -------------
fsm_test = executable('test_reac_fsm',
  ['tests/test_reac_fsm.c', 'src/reac_fsm.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep])
test('reac_fsm', fsm_test)

# --- unit test: the MASTER-role handshake (cdea/cfea bytes vs the captures) ----
# Asserts the GENERATED channel-map / cfea (from the S-1608 console config) match
# the captured M-300 bytes + satisfy the checksum, the fixed probe/sub01/sub02
# constants are byte-exact, the grant echoes the JOIN, and the FSM sequences
# IDLE->...->ESTABLISHED with the continuous 5-message cadence in both states.
master_test = executable('test_reac_master',
  ['tests/test_reac_master.c', 'src/reac_master.c', 'src/reac_master_fsm.c',
   'src/reac_ctrl.c', 'src/reac_grant.c', 'src/reac_headamp_tx.c',
   'src/reac_tx.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, m_dep])
test('reac_master', master_test)

# --- unit test: the MASTER establishment decision core (pure table) ------------
# Every (state, event) pair of the transition table — all 4 x 11 = 44, ignores
# included — asserted against docs/MASTER-FSM.md, plus the classifier fold
# (rx kind x have-block/same-box/grant-delivered guards). Pure — no socket, no
# struct reac_master.
master_fsm_test = executable('test_reac_master_fsm',
  ['tests/test_reac_master_fsm.c', 'src/reac_master_fsm.c'],
  include_directories : [include_directories('src')])
test('reac_master_fsm', master_fsm_test)

# --- acceptance test: the S-1608 downstream is byte-exact vs the M-300 capture -
# Feeds the generator the S-1608 config and asserts the generated CHANMAP equals
# the captured M-300 chanmap EXACTLY, the generated CFEA equals it except our MAC
# + checksum (and is fully byte-exact when our MAC is the M-300's), and every
# generated control block sums to 0 mod 256. The #130 hardware-verify gate.
s1608_test = executable('test_reac_s1608',
  ['tests/test_reac_s1608.c', 'src/reac_master.c', 'src/reac_master_fsm.c',
   'src/reac_ctrl.c', 'src/reac_grant.c', 'src/reac_headamp_tx.c', 'src/reac_tx.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, m_dep])
test('reac_s1608', s1608_test)

# --- unit test: the ProcessLatency smoother (pure EMA + hysteresis) -----------
# No PipeWire, no socket: feeds a 38..113 depth sawtooth through the EMA and
# asserts the smoothed latency settles into a tight band and stops re-advertising.
lat_test = executable('test_reac_lat',
  ['tests/test_reac_lat.c', 'src/reac_lat.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, m_dep])
test('reac_lat', lat_test)

# --- unit test: the SCHED_FIFO cadence pacer (ring + interval) -----------------
# No socket, no RT privilege needed: tests the frame ring SPSC behaviour, the
# per-rate slot period (125/250/272 us), and the master-stamp-on-egress path.
pacer_test = executable('test_reac_pacer',
  ['tests/test_reac_pacer.c', 'src/reac_pacer.c', 'src/reac_disco.c', 'src/reac_master.c',
   'src/reac_master_fsm.c', 'src/reac_ctrl.c', 'src/reac_grant.c', 'src/reac_mac.c',
   'src/reac_headamp_tx.c', 'src/reac_clock.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, threads_dep, m_dep])
test('reac_pacer', pacer_test)

# --- unit test: the clock-discipline core — hierarchy + the period DLL (#75) ---
# Pure: ppm in, period out. No socket, no PipeWire, no clock read. Pins the
# ROLE-DEPENDENT lock hierarchy (master: phc > graph > freerun, with a slaved box
# deliberately absent; slave: the wire and nothing else), the DLL's convergence /
# absence of overshoot / slew + clamp bounds / rejection of insane samples, honest
# degradation (free-run is never called locked; a vanished reference HOLDS the last
# good rate), and a replay of a noisy counter series that must not be chased.
# Since #77 it also pins reference QUALITY: the name heuristic may only REJECT
# (an unrecognised professional interface must never be demoted), an operator
# designation outranks the heuristic, the DLL's measured residual variance
# outranks both, and our own estimator's noise floor sits provably inside the
# "we cannot tell" band so it can never be reported as an unstable reference.
clock_test = executable('test_reac_clock',
  ['tests/test_reac_clock.c', 'src/reac_clock.c'],
  include_directories : [include_directories('src')],
  dependencies : [m_dep])
test('reac_clock', clock_test)

# --- unit test: the pacer's clock-discipline WIRING (#75) ---------------------
# No socket, no RT privilege: drives reac_pacer_clock_publish / _clock_tick the way
# the publishers and the RT thread do. Proves INERTNESS first (knob unset -> every
# tick returns the nominal period, the discipline is never advanced, the transcript
# is unchanged), then the two references the rig actually uses to the same standard:
# the elected PipeWire driver (graph) and a stagebox that is itself the clock master
# (box) — including the closed-loop case of a box slaved to US, where the cadence
# must park rather than run away. Also the label seqlock and the reference hierarchy
# end to end, and (#77) the quality path: a hardware-but-display-derived graph clock
# losing to the box, an operator-designated reference reported by tier as well as by
# name, and a measurably jittery one flagged rather than ejected.
pacer_clock_test = executable('test_reac_pacer_clock',
  ['tests/test_reac_pacer_clock.c', 'src/reac_pacer.c', 'src/reac_clock.c',
   'src/reac_disco.c', 'src/reac_master.c', 'src/reac_master_fsm.c',
   'src/reac_ctrl.c', 'src/reac_grant.c', 'src/reac_mac.c', 'src/reac_headamp_tx.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, threads_dep, m_dep])
test('reac_pacer_clock', pacer_clock_test)

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
   'src/reac_ctrl.c', 'src/reac_ring.c', 'src/reac_mac.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, threads_dep, m_dep])
test('reac_slave', slave_test)

# --- unit test: virtual-stagebox head-amp -> input gain (task #202) ------------
# The slave APPLIES received SENS/PAD as per-input digital gain on the upstream
# audio (a virtual box has no preamp). Pins the gain model, the wire-CH -> input
# mapping, and the RT multiply-only apply. Same link set as slave_test.
vboxgain_test = executable('test_reac_vboxgain',
  ['tests/test_reac_vboxgain.c', 'src/reac_slave.c', 'src/reac_fsm.c',
   'src/reac_ctrl.c', 'src/reac_ring.c', 'src/reac_mac.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, threads_dep, m_dep])
test('reac_vboxgain', vboxgain_test)

# --- unit test: the full offline COURTSHIP (our master <-> our slave) ----------
# Both establishment cores are pure, so the whole M-5000 <-> S-1608 sequence runs
# as a frame-by-frame simulation with no socket: master emissions (next+stamp on
# reac_downstream_build FILLERs) feed reac_slave_step_rx; slave emissions (the reac_ctrl
# builders) feed reac_ctrl_classify_box_frame + reac_master_rx. Pins the golden
# ordering (probe -> box JOIN -> echoed grant -> box unicast -> both ESTABLISHED
# -> HOLD -> peer-gone at 600) AND the matcher/builder mutual compatibility.
courtship_test = executable('test_reac_courtship',
  ['tests/test_reac_courtship.c', 'src/reac_master.c', 'src/reac_master_fsm.c',
   'src/reac_slave.c', 'src/reac_fsm.c', 'src/reac_ctrl.c', 'src/reac_grant.c',
   'src/reac_headamp_tx.c', 'src/reac_ring.c', 'src/reac_tx.c', 'src/reac_mac.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, threads_dep, m_dep])
test('reac_courtship', courtship_test)

# --- unit test: the output-gain staging kernel (pure ramp, no PipeWire) --------
# Exercises reac_gain_ramp_block directly: unity is a no-op, a target is reached
# within the expected sample count, mute ramps to zero, and two channels ramp
# independently. No socket/PipeWire — the pure kernel the RT sink path calls.
gain_test = executable('test_reac_gain',
  ['tests/test_reac_gain.c', 'src/reac_gain.c'],
  include_directories : [include_directories('src')])
test('reac_gain', gain_test)

# --- unit test: the reac.link-state property mapping (pure, no PipeWire) ------
# Pins reac_link_state_from_master: every reac_master_state -> its badge string
# (IDLE folded into "probing"), the just_dropped overlay, and the exact wire
# strings a consumer (openmixer's stagebox card) matches on.
link_state_test = executable('test_reac_link_state',
  ['tests/test_reac_link_state.c', 'src/reac_link_state.c'],
  include_directories : [include_directories('src')])
test('reac_link_state', link_state_test)

# --- unit test: passive discovery — the sighting classifier + table ------------
# Pins the bars that make a device list trustworthy: presence ONLY from a real
# 0x8819 Roland-OUI frame (never a packet count — openmixer #161), role from the
# full byte signature rather than the frame kind (cdea 04 03 is both the box JOIN
# and the master grant; cdea 01 03 0010 is a box frame parsing as kind PROBE), a
# model only ever from the byte-exact config-block match, stale devices withdrawn,
# and all-or-nothing JSON. Frames come from the reac_ctrl builders, so the test
# runs on the same bytes the wire carries.
disco_test = executable('test_reac_disco',
  ['tests/test_reac_disco.c', 'src/reac_disco.c', 'src/reac_ctrl.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, m_dep])
test('reac_disco', disco_test)

# --- unit test: --role selection parsing (master default, slave needs --tx) -----
# Compiles a tiny harness that exercises the same role-parse + validation logic
# main.c uses, with no PipeWire / socket. Asserts: default is master; --role slave
# is parsed; --role slave without --tx is rejected; an unknown role is rejected.
role_test = executable('test_reac_role',
  ['tests/test_reac_role.c'],
  include_directories : [include_directories('src')])
test('reac_role', role_test)

# --- unit test: the stand-in source-MAC helper (task #35, D.10) ---------------
# Pure reac_mac_compose (Roland OUI + NIC host part; box/desk-safe fallback) — no
# NIC touched. Proves the derived default can never equal a real box's MAC.
mac_test = executable('test_reac_mac',
  ['tests/test_reac_mac.c', 'src/reac_mac.c'],
  include_directories : [include_directories('src')])
test('reac_mac', mac_test)

# --- unit test: the MASTER head-amp DMX send scheduler (task #155, C.7) --------
# Pure state table + edge/periodic-re-assert scheduler — no socket, no FSM. Pins
# the edge-on-change emission, the full-table re-assert sweep, and OFF-unless-set.
headamp_tx_test = executable('test_reac_headamp_tx',
  ['tests/test_reac_headamp_tx.c', 'src/reac_headamp_tx.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep])
test('reac_headamp_tx', headamp_tx_test)

# --- unit test: parse LIVE head-amp control out of a SPA_PARAM_Props pod (#203) -
# Pure SPA-pod parse (no PipeWire, no graph): builds Props objects carrying
# SPA_PROP_params keyed "reac.headamp.<ch>.<param>" and asserts the decode, the
# per-param range gate, the Bool/Int/Float encodings, and (key,value) resync.
headamp_prop_test = executable('test_reac_headamp_prop',
  ['tests/test_reac_headamp_prop.c', 'src/reac_headamp_prop.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, spa_dep])
test('reac_headamp_prop', headamp_prop_test)

# --- unit test: the LIVE head-amp control handoff (#203) -----------------------
# The lock-free SPSC command ring + drain that carries a per-channel change from
# the PipeWire main loop to the RT pacer thread. Drives the pacer's ring + table
# directly (no socket/RT privilege): functional queue->drain->emit, ring-full
# drop, and a writer-thread stress that asserts NO torn (ch,param,value) triple.
headamp_live_test = executable('test_reac_headamp_live',
  ['tests/test_reac_headamp_live.c', 'src/reac_pacer.c', 'src/reac_disco.c',
   'src/reac_master.c', 'src/reac_master_fsm.c', 'src/reac_ctrl.c',
   'src/reac_grant.c', 'src/reac_mac.c', 'src/reac_headamp_tx.c', 'src/reac_clock.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, threads_dep, m_dep])
test('reac_headamp_live', headamp_live_test)

# --- unit test: the GRANT enrollment sweep — allocator + generated group A -----
# The fix for "link established != channels enrolled" (GRANT-SWEEP.md, 2026-07-17).
# Pins the slot ALLOCATOR (observed bases, the 0x2f fabric ceiling), the sweep SHAPE
# (width x 3 group-A records, params 0/1/2 per channel, channels contiguous from
# base), byte-exactness of every generated record against the proven head-amp
# builder, the value-sourcing rule (head-amp table when set, safe default otherwise),
# and that group B stays the fixed width-invariant constant. ALSO pins the two slot
# spaces apart (#69): the same base-32 x width-16 span is LEGAL as head-amp (an
# S-1608 reaching CH 47) and ILLEGAL as audio (past the 40 slots the frame carries),
# which is why reac_boxreg links in here. Pure — no socket.
grant_test = executable('test_reac_grant',
  ['tests/test_reac_grant.c', 'src/reac_grant.c', 'src/reac_ctrl.c',
   'src/reac_headamp_tx.c', 'src/reac_boxreg.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, m_dep])
test('reac_grant', grant_test)

# --- acceptance test: byte conformance across ALL --mixer profiles ------------
# The finding: m200/m300/m5000 differ on the wire in exactly TWO bytes — the
# source MAC and the cfea console_field byte (V-Mixer 0x00 / OHRCA 0x01), which
# also drives the ENROLL console byte. Drives the real reac_master_init/set_box/
# stamp path for each profile and asserts the per-console cfea/ENROLL identity
# bytes against a new golden (reac_conformance_golden.inc) AND that the shared
# generator (chanmap/probe/grant sweep) still reproduces the existing M-200/
# M-300 goldens (reac_m200_golden.inc, reac_grant_golden.inc) regardless of
# profile. Pure — no socket.
conformance_test = executable('test_reac_conformance',
  ['tests/test_reac_conformance.c', 'src/reac_master.c', 'src/reac_master_fsm.c',
   'src/reac_ctrl.c', 'src/reac_grant.c', 'src/reac_tx.c', 'src/reac_headamp_tx.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, m_dep])
test('reac_conformance', conformance_test)
