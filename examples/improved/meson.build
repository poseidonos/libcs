# SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
#
# SPDX-License-Identifier: BSD-3-Clause

improved_sources = [
  'scenario_01_improved.c',
  'scenario_02_improved.c',
  'scenario_03_improved.c',
  'scenario_04_improved.c',
]

foreach src : improved_sources
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
