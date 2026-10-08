# SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
#
# SPDX-License-Identifier: BSD-3-Clause

bench_filter_exe = executable(
  'bench_filter',
  'bench_filter.c',
  include_directories: include_directories('..', '../include'),
  link_with: libcs,
  dependencies: [thread_dep],
  install: false,
)

bench_compress_exe = executable(
  'bench_compress',
  'bench_compress.c',
  include_directories: include_directories('..', '../include'),
  link_with: libcs,
  dependencies: [thread_dep],
  install: false,
)

bench_dot_product_exe = executable(
  'bench_dot_product',
  'bench_dot_product.c',
  include_directories: include_directories('..', '../include'),
  link_with: libcs,
  dependencies: [thread_dep],
  install: false,
)

bench_cosine_similarity_exe = executable(
  'bench_cosine_similarity',
  'bench_cosine_similarity.c',
  include_directories: include_directories('..', '../include'),
  link_with: libcs,
  dependencies: [thread_dep, math_dep],
  install: false,
)

# Hardware benchmark: requires a live target URI (--uri traddr:port), so it is
# built but deliberately NOT part of the run_benchmarks custom_target.
bench_compute_from_nvm_exe = executable(
  'bench_compute_from_nvm',
  'bench_compute_from_nvm.c',
  include_directories: include_directories('..', '../include'),
  link_with: libcs,
  dependencies: [thread_dep, math_dep],
  install: false,
)

custom_target(
  'run_benchmarks',
  output: ['bench_filter.out', 'bench_compress.out', 'bench_dot_product.out', 'bench_cosine_similarity.out'],
  command: [
    find_program('sh'),
    '-c',
    'mkdir -p benchmark_results && @0@ > benchmark_results/bench_filter.out && @1@ > benchmark_results/bench_compress.out && @2@ > benchmark_results/bench_dot_product.out && @3@ > benchmark_results/bench_cosine_similarity.out && cp benchmark_results/bench_filter.out @OUTPUT0@ && cp benchmark_results/bench_compress.out @OUTPUT1@ && cp benchmark_results/bench_dot_product.out @OUTPUT2@ && cp benchmark_results/bench_cosine_similarity.out @OUTPUT3@'.format(
      bench_filter_exe.full_path(),
      bench_compress_exe.full_path(),
      bench_dot_product_exe.full_path(),
      bench_cosine_similarity_exe.full_path()
    )
  ],
  depends: [bench_filter_exe, bench_compress_exe, bench_dot_product_exe, bench_cosine_similarity_exe],
  build_by_default: false,
)
