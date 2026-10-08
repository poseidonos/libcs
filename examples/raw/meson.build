# SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
#
# SPDX-License-Identifier: BSD-3-Clause

raw_sources = [
  'scenario_01_raw.c',
  'scenario_02_raw.c',
  'scenario_03_raw.c',
  'scenario_04_raw.c',
]

foreach src : raw_sources
  exe_name = src.split('.')[0]
  executable(
    exe_name,
    src,
    include_directories: include_directories('../..', '../../include'),
    link_with: libcs,
    dependencies: [thread_dep],
    install: false,
  )
endforeach
