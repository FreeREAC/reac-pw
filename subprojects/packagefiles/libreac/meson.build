# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# meson shim for libreac (FreeREAC/libreac ships only a hand Makefile). Dropped
# into the libreac checkout by reac-pw's subprojects/libreac.wrap patch_directory.
# Builds the static wire-format core and exposes libreac_dep with <reac/reac.h>.
# reac_encode.c rounds with lrintf, so the dependency carries libm.
#
# The version below is what reac-pw's `dependency('libreac', version: ...)`
# floor is checked against when the fallback is taken, so it has to track the
# libreac the wrap actually clones (`revision = main`). Keep it in step with
# packaging/libreac.spec upstream. The version string alone proves nothing — it is
# hardcoded here, so a stale checkout reports it too. What a stale checkout cannot
# do is configure: src/reac_ctrlblk.c is listed below and does not exist before it.

project('libreac', 'c',
  version : '0.6.0',
  license : 'GPL-3.0-or-later',
  default_options : ['c_std=c11', 'warning_level=2'])

libreac_inc = include_directories('include')
libreac_m = meson.get_compiler('c').find_library('m', required : false)

libreac_lib = static_library('reac',
  ['src/reac.c', 'src/reac_decode.c', 'src/reac_upstream.c', 'src/reac_encode.c',
   'src/reac_capture.c', 'src/pcap_source.c', 'src/reac_ports.c',
   'src/reac_ctrlblk.c'],
  include_directories : libreac_inc,
  dependencies : [libreac_m],
  install : false)

libreac_dep = declare_dependency(
  link_with : libreac_lib,
  dependencies : [libreac_m],
  include_directories : libreac_inc)
