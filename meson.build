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
  version : '0.4.3',
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
libreac_dep = dependency('libreac', version : '>=0.7.0',
  fallback : ['libreac', 'libreac_dep'],
  required : true)

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
  'src/reac_ring.c',
  'src/reac_rx.c',
  'src/reac_pace_watch.c',
  'src/reac_box_pin.c',
  'src/reac_seglock.c',
  'src/reac_arbitration.c',
  'src/reac_conf.c',
  'src/reac_tx.c',
  'src/reac_ctrl.c', 'src/reac_scene_body.c',
  'src/reac_fsm.c',
  'src/reac_grant.c',
  'src/reac_master.c',
  'src/reac_master_fsm.c',
  'src/reac_pacer.c',
  'src/reac_slave.c',
  'src/reac_mac.c',
  'src/reac_headamp_tx.c',
  'src/reac_headamp_prop.c',
  'src/reac_rate_cfg.c',
  'src/reac_role_cfg.c',
  'src/reac_sink_format.c',
  'src/reac_source_node.c',
  'src/reac_sink_node.c',
  'src/reac_lat.c',
  'src/reac_gain.c',
  'src/reac_boxreg.c',
  'src/reac_link_state.c',
  'src/reac_disco.c',
  'src/reac_clock.c',
]

reac_pw_exe = executable('reac-pw',
  reac_pw_src,
  dependencies : [pipewire_dep, spa_dep, threads_dep, m_dep, libreac_dep],
  install : true)

# --- unit test: the layered-config PRECEDENCE law ----------------------------
# Pins the override order declared in docs/RATE-AND-CLOCK-CONFIG.md against the
# code that implements it. The two drifting apart is the failure this file exists
# to make impossible: a chain whose order lives only in prose is a chain every
# reader infers differently, and they are all sure they are right.
conf_test = executable('test_reac_conf',
  ['tests/test_reac_conf.c', 'src/reac_conf.c'],
  include_directories : [include_directories('src')])
test('reac_conf', conf_test)

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
  ['tests/test_reac_encode_golden.c', 'src/reac_ctrl.c', 'src/reac_scene_body.c'],
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
  ['tests/test_reac_rx_gate.c', 'src/reac_rx.c', 'src/reac_ring.c',
   'src/reac_pace_watch.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, threads_dep, m_dep])
test('reac_rx_gate', rx_gate_test)

# --- unit test: the OHRCA duplicate-frame guard (frames_dup) ------------------
# Runs the real feeder thread over a temp pcap of DOUBLED byte-identical
# delivery (the mirrored-RX-path / doubled-cadence granulated-audio bug):
# exactly one twin dropped per frame, single-cadence ring, and a negative —
# distinct frames at doubled cadence pass untouched (dup == 0).
rx_dup_test = executable('test_reac_rx_dup',
  ['tests/test_reac_rx_dup.c', 'src/reac_rx.c', 'src/reac_ring.c',
   'src/reac_pace_watch.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, threads_dep, m_dep])
test('reac_rx_dup', rx_dup_test)

# --- unit test: --live refuses a vanished/nonexistent interface ---------------
# A dead, renamed or never-existed --live NIC used to be discovered only INSIDE
# the feeder thread (silently — the thread just returns NULL) whenever --rate
# was also given, because reac_rx_open() skipped opening the capture at all in
# that case. The deployed unit always passes --rate. No privilege or live
# traffic needed: a made-up interface name is refused the same way as a
# genuinely dead one.
rx_live_iface_test = executable('test_reac_rx_live_iface',
  ['tests/test_reac_rx_live_iface.c', 'src/reac_rx.c', 'src/reac_ring.c',
   'src/reac_pace_watch.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, threads_dep, m_dep])
test('reac_rx_live_iface', rx_live_iface_test)

# --- unit test: control-plane builders/parser/checksum (ground-truthed) -------
ctrl_test = executable('test_reac_ctrl',
  ['tests/test_reac_ctrl.c', 'src/reac_ctrl.c', 'src/reac_scene_body.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, m_dep])
test('reac_ctrl', ctrl_test)

# --- unit test: head-amp source control (op 04 03 TAG 01 01, M-200-truthed) ---
headamp_test = executable('test_reac_headamp',
  ['tests/test_reac_headamp.c', 'src/reac_ctrl.c', 'src/reac_scene_body.c'],
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
   'src/reac_ctrl.c', 'src/reac_scene_body.c', 'src/reac_grant.c', 'src/reac_headamp_tx.c',
   'src/reac_tx.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, m_dep])
test('reac_master', master_test)

# --- rig-test switch: REACPW_NO_ENROLL removes exactly one frame, nothing else ---
# Its own process: the knob is cached on first getenv() read, like this file's others.
master_no_enroll_test = executable('test_reac_master_no_enroll',
  ['tests/test_reac_master_no_enroll.c', 'src/reac_master.c', 'src/reac_master_fsm.c',
   'src/reac_ctrl.c', 'src/reac_scene_body.c', 'src/reac_grant.c', 'src/reac_headamp_tx.c',
   'src/reac_tx.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, m_dep])
test('reac_master_no_enroll', master_no_enroll_test)

# --- unit test: the MASTER establishment decision core (pure table) ------------
# Every (state, event) pair of the transition table — all 4 x 11 = 44, ignores
# included — asserted against docs/MASTER-FSM.md, plus the classifier fold
# (rx kind x have-block/same-box/grant-delivered guards). Pure — no socket, no
# struct reac_master.
master_fsm_test = executable('test_reac_master_fsm',
  ['tests/test_reac_master_fsm.c', 'src/reac_master_fsm.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep])
test('reac_master_fsm', master_fsm_test)

# --- acceptance test: the S-1608 downstream is byte-exact vs the M-300 capture -
# Feeds the generator the S-1608 config and asserts the generated CHANMAP equals
# the captured M-300 chanmap EXACTLY, the generated CFEA equals it except our MAC
# + checksum (and is fully byte-exact when our MAC is the M-300's), and every
# generated control block sums to 0 mod 256. The #130 hardware-verify gate.
s1608_test = executable('test_reac_s1608',
  ['tests/test_reac_s1608.c', 'src/reac_master.c', 'src/reac_master_fsm.c',
   'src/reac_ctrl.c', 'src/reac_scene_body.c', 'src/reac_grant.c', 'src/reac_headamp_tx.c', 'src/reac_tx.c'],
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
   'src/reac_master_fsm.c', 'src/reac_ctrl.c', 'src/reac_scene_body.c', 'src/reac_grant.c', 'src/reac_mac.c',
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
   'src/reac_ctrl.c', 'src/reac_scene_body.c', 'src/reac_grant.c', 'src/reac_mac.c', 'src/reac_headamp_tx.c'],
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
   'src/reac_ctrl.c', 'src/reac_scene_body.c', 'src/reac_ring.c', 'src/reac_mac.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, threads_dep, m_dep])
test('reac_slave', slave_test)

# --- unit test: virtual-stagebox head-amp -> input gain (task #202) ------------
# The slave APPLIES received SENS/PAD as per-input digital gain on the upstream
# audio (a virtual box has no preamp). Pins the gain model, the wire-CH -> input
# mapping, and the RT multiply-only apply. Same link set as slave_test.
vboxgain_test = executable('test_reac_vboxgain',
  ['tests/test_reac_vboxgain.c', 'src/reac_slave.c', 'src/reac_fsm.c',
   'src/reac_ctrl.c', 'src/reac_scene_body.c', 'src/reac_ring.c', 'src/reac_mac.c'],
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
   'src/reac_slave.c', 'src/reac_fsm.c', 'src/reac_ctrl.c', 'src/reac_scene_body.c', 'src/reac_grant.c',
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
# --- source gate: every dequeued pw_buffer is queued back, on every path -------
# Not a unit test: the defect lives in a PipeWire callback no unit test reaches,
# and it fails in the way that hides best — the sink re-sends one stale frame
# forever at exactly the right rate with a valid non-zero audio region, so every
# check except "does this frame differ from the last one" reports health.
python3 = find_program('python3', required : true)
test('sink_buffer_pairing', python3,
  args : [meson.current_source_dir() / 'tests' / 'test_sink_buffer_pairing.py'])

link_state_test = executable('test_reac_link_state',
  ['tests/test_reac_link_state.c', 'src/reac_link_state.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep])
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
  ['tests/test_reac_disco.c', 'src/reac_disco.c', 'src/reac_ctrl.c', 'src/reac_scene_body.c'],
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

# --- unit test: who drives this segment (arbitration increment 2) -------------
# The S-4000S incident: a box in slave mode sighted as role `master`, and nothing anywhere
# added the evidence up into a statement about the segment. Passive — decides nothing.
arbitration_test = executable('test_reac_arbitration',
  ['tests/test_reac_arbitration.c', 'src/reac_arbitration.c', 'src/reac_disco.c',
   'src/reac_ctrl.c', 'src/reac_scene_body.c'],
  dependencies : [libreac_dep],
  include_directories : [include_directories('src')])
test('reac_arbitration', arbitration_test)

# --- unit test: --box MODEL[:LABEL], the fixed-installation pin ---------------
# Restored 2026-08-22 (operator: reac-pw is a daemon, a permanent rig pins its patch;
# openmixer autodetects — complementary, not alternatives). The pin declares NODE
# GEOMETRY AND LABEL only: --box was retired in 902bf39 because it pre-fabricated a
# head-amp enrollment from a guess, and a box granted slots it does not own links,
# streams audio and silently ignores every head-amp record.
box_pin_test = executable('test_reac_box_pin',
  ['tests/test_reac_box_pin.c', 'src/reac_box_pin.c',
  'src/reac_seglock.c', 'src/reac_ctrl.c', 'src/reac_scene_body.c'],
  dependencies : [libreac_dep],
  include_directories : [include_directories('src')])
test('reac_box_pin', box_pin_test)

# --- unit test: does the wire carry the pace we CLAIM it does? ----------------
# Rig 2026-08-22: --rate 96000 gave a byte-perfect 7996 pps downstream (1492-byte frames,
# 125.1 us mean) and BOTH boxes answered 4000 pps, while the capture node was published as
# 96000 Hz over 48 000 samples/s with gaps=0 and PipeWire ERR 0. Needs libreac for
# reac_rate_snap (pps -> pace), which until now had no callers anywhere.
pace_watch_test = executable('test_reac_pace_watch',
  ['tests/test_reac_pace_watch.c', 'src/reac_pace_watch.c'],
  dependencies : [libreac_dep],
  include_directories : [include_directories('src')])
test('reac_pace_watch', pace_watch_test)

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
   'src/reac_master.c', 'src/reac_master_fsm.c', 'src/reac_ctrl.c', 'src/reac_scene_body.c',
   'src/reac_grant.c', 'src/reac_mac.c', 'src/reac_headamp_tx.c', 'src/reac_clock.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, threads_dep, m_dep])
test('reac_headamp_live', headamp_live_test)

# --- unit test: reac.cfg.rate — closed list, drivability, decision core, and --
# the internal re-establish (2026-08-26-reac-runtime-config.md) ---------------
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
  ['tests/test_reac_rate_cfg.c', 'src/reac_rate_cfg.c', 'src/reac_pacer.c',
   'src/reac_disco.c', 'src/reac_master.c', 'src/reac_master_fsm.c', 'src/reac_ctrl.c',
   'src/reac_scene_body.c', 'src/reac_grant.c', 'src/reac_mac.c', 'src/reac_headamp_tx.c',
   'src/reac_clock.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, threads_dep, m_dep, spa_dep])
test('reac_rate_cfg', rate_cfg_test)

# --- unit test: reac.cfg.role — the ROLE half of runtime config ---------------
# (2026-08-26-reac-runtime-config.md). Pure decision core only (reac_role_cfg.c:
# the parse, the same-role-is-a-no-op detection, the state string a request
# publishes) — there is no pacer-level re-establish to exercise here at all,
# because this increment's own honesty clause (reac_role_cfg.h) is that the
# cross-engine swap a role CHANGE would need (master's reac_sink_node+
# reac_pacer vs. a slave's reac_slave — two different engines, each owning its
# own AF_PACKET socket) is a documented stub: it is decided and answered, never
# silently dropped and never faked as applied, but not actually carried out
# without a real NIC to prove the re-attach against. Proves: role 0/1 parse
# (Int and the Float-bridge tolerance), a malformed value refuses, a same-role
# assertion is an idempotent "applied" no-op, and a role-changing assertion
# answers "role_reestablish_pending" — and stays there, sabotage-verified by
# flipping the state string comparison in reac_role_cfg_apply_state to prove
# the shape-identity test actually distinguishes the two answers.
role_cfg_test = executable('test_reac_role_cfg',
  ['tests/test_reac_role_cfg.c', 'src/reac_role_cfg.c'],
  include_directories : [include_directories('src')],
  dependencies : [spa_dep])
test('reac_role_cfg', role_cfg_test)

# --- unit test: reac_sink_format — the reac-playback node's Format pod and ---
# the renegotiate decision (2026-08-26-clock-tabs-and-reac-pace-coupling.md
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
  include_directories : [include_directories('src')],
  dependencies : [spa_dep, libreac_dep])
test('reac_sink_format', sink_format_test)

# --- unit test: reac_source_node's rate renegotiation (one wire, one rate) ---
# (2026-08-26-clock-tabs-and-reac-pace-coupling.md §1b: "capture AND playback
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
  include_directories : [include_directories('src')],
  dependencies : [spa_dep, libreac_dep])
test('reac_source_format', source_format_test)

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
  ['tests/test_reac_grant.c', 'src/reac_grant.c', 'src/reac_ctrl.c', 'src/reac_scene_body.c',
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
   'src/reac_ctrl.c', 'src/reac_scene_body.c', 'src/reac_grant.c', 'src/reac_tx.c', 'src/reac_headamp_tx.c'],
  include_directories : [include_directories('src')],
  dependencies : [libreac_dep, m_dep])
test('reac_conformance', conformance_test)

# --- benchmark (not a test): per-frame CPU cost of the two hot paths ----------
# Built always so it cannot rot, run by hand — it is a measurement, not a gate.
# It `#include`s src/reac_rx.c to reach the static feed_frame, which is the only
# way to benchmark the SHIPPING decode path instead of a copy of it that would
# drift the moment either one changed.
bench_hotpath = executable('bench_hotpath',
  ['tools/bench_hotpath.c', 'src/reac_ring.c', 'src/reac_pace_watch.c',
   'src/reac_pacer.c', 'src/reac_disco.c', 'src/reac_master.c',
   'src/reac_master_fsm.c', 'src/reac_ctrl.c', 'src/reac_scene_body.c',
   'src/reac_grant.c', 'src/reac_mac.c', 'src/reac_headamp_tx.c', 'src/reac_clock.c'],
  include_directories : [include_directories('src'), include_directories('tests')],
  dependencies : [libreac_dep, threads_dep, m_dep],
  build_by_default : true)

# --- CLI text: --headamp's advertised CH bound must match the head-amp space ---
# (REAC_HEADAMP_MAX_CH, 48 slots — an S-1608 at base 0x20 occupies 32..47) rather
# than the audio-fabric width (40, wire channels 0..39). Runs the built binary
# with no args (usage()) and reads its own stderr, so a future edit of the help
# string is caught the same way an operator misreading it would be.
usage_test = executable('test_reac_headamp_usage',
  ['tests/test_reac_headamp_usage.c'])
test('reac_headamp_usage', usage_test, args : [reac_pw_exe.full_path()])
