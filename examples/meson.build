# SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
#
# SPDX-License-Identifier: BSD-3-Clause

example_sources = [
  'cs_vector_bench.c',
]

foreach src : example_sources
  exe_name = src.split('.')[0]
  executable(
    exe_name,
    src,
    include_directories: include_directories('..', '../include'),
    link_with: libcs,
    dependencies: [thread_dep],
    install: false,
  )
endforeach

subdir('improved')
subdir('raw')
