# SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
#
# SPDX-License-Identifier: BSD-3-Clause

project(
  'libcs',
  'c',
  version: '1.0.0',
  license: 'BSD-3-Clause',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=release',
    'default_library=both',
  ],
  meson_version: '>=0.58'
)

# Import modules
pconf = import('pkgconfig')

# Compiler setup
cc = meson.get_compiler('c')
cc_flags = [
  '-Wno-missing-braces',
  '-Wno-unused-parameter',
]
add_project_arguments(
  cc.get_supported_arguments(cc_flags),
  language: 'c'
)

if get_option('sim_backend')
  add_project_arguments('-DWITH_SIM_BACKEND', language: 'c')
endif

if get_option('ioctl_backend')
  add_project_arguments('-DWITH_IOCTL_BACKEND', language: 'c')
endif

if get_option('io_uring_backend')
  add_project_arguments('-DWITH_IO_URING_BACKEND', language: 'c')
endif

if get_option('libnvme_backend')
  add_project_arguments('-DWITH_LIBNVME_BACKEND', language: 'c')
endif

# Version info
version_major = meson.project_version().split('.')[0]
version_minor = meson.project_version().split('.')[1]
version_patch = meson.project_version().split('.')[2]

# Configuration
conf_data = configuration_data()
conf_data.set('CS_VERSION_MAJOR', version_major)
conf_data.set('CS_VERSION_MINOR', version_minor)
conf_data.set('CS_VERSION_PATCH', version_patch)
conf_data.set('CS_DEBUG_ENABLED', get_option('buildtype') == 'debug')

# Dependencies
thread_dep = dependency('threads', required: true)
math_dep = cc.find_library('m', required: false)

# xNVMe dependency — DEV-ONLY, and no longer part of libcs itself.
#
# As of commit 6/10 of the xNVMe -> direct-SPDK port the library does not
# link xNVMe at all; `xnvme_dep` survives for exactly one consumer, the
# CP/SLM command-builder differential oracle (tests/test_cmd_oracle.c),
# which builds each of the 7 commands with BOTH the fork's builders and
# cs_nvme_cmds.c's and memcmp's the 64-byte results. Commit 10 deletes the
# oracle and this whole block with it.
#
# Always use the sibling build (NOT system pkg-config) to avoid linking
# against an outdated system libxnvme.so that may lack recent symbols
# (e.g. xnvme_cmd_slm_read).

xnvme_source_dir = meson.project_source_root() / '..' / 'xnvme'
xnvme_build_dir = xnvme_source_dir / 'build'
xnvme_include_dir = xnvme_source_dir / 'include'
xnvme_lib_dir = xnvme_build_dir / 'lib'

# Look for built library in build/lib/
xnvme_lib_found = false
xnvme_lib = ''

if run_command('test', '-f', xnvme_lib_dir / 'libxnvme.so', check: false).returncode() == 0
  xnvme_lib = cc.find_library('xnvme', dirs: [xnvme_lib_dir], required: true)
  xnvme_lib_found = true
  message('Using xNVMe from ' + xnvme_lib_dir / 'libxnvme.so')
elif run_command('test', '-f', xnvme_lib_dir / 'libxnvme.dylib', check: false).returncode() == 0
  xnvme_lib = cc.find_library('xnvme', dirs: [xnvme_lib_dir], required: true)
  xnvme_lib_found = true
  message('Using xNVMe from ' + xnvme_lib_dir / 'libxnvme.dylib')
elif run_command('test', '-f', xnvme_lib_dir / 'libxnvme.a', check: false).returncode() == 0
  xnvme_lib = cc.find_library('xnvme', dirs: [xnvme_lib_dir], required: true)
  xnvme_lib_found = true
  message('Using xNVMe from ' + xnvme_lib_dir / 'libxnvme.a')
endif

if xnvme_lib_found
  xnvme_dep = declare_dependency(
    compile_args: ['-I' + xnvme_include_dir],
    dependencies: [xnvme_lib, thread_dep]
  )
else
  xnvme_dep = dependency('', required: false)
endif

# SPDK dependency discovery (commit 4/10 of the xNVMe -> direct-SPDK port,
# see ~/.claude/plans/hidden-sprouting-turtle.md). Single source of truth
# for "how do we find SPDK".
#
# Two DISTINCT signals come out of this block, and conflating them breaks
# builds (commit 6/10):
#
#   spdk_dep      — "SPDK headers are available", true for ANY discovery
#                   tier below, including the headers-only sibling fallback
#                   (tier 3) where no SPDK library exists at all. Feeds only
#                   the compile-only check target (cs_nvme_spdk_compile_check).
#                   That is safe because archiving a static_library never
#                   resolves undefined symbols, so headers alone suffice.
#
#   spdk_linkable — "a real SPDK library exists to link against": tier 1
#                   (pkg-config) or tier 2 (a real archive/shared object in
#                   the sibling build tree) only. `spdk_link_dep` carries the
#                   corresponding dependency object.
#
# ONLY spdk_linkable may gate the real libcs library's SPDK-backed sources.
# libcs is built with default_library=both, so meson also links a SHARED
# library, and that link DOES have to resolve every spdk_nvme_* /
# spdk_dma_* / spdk_vtophys symbol cs_nvme_spdk.c references. Gating the
# real library on spdk_dep.found() would therefore break every box that has
# SPDK headers but no built SPDK — including macOS, where DPDK (and hence
# SPDK) does not build at all, and where tier 3 is the only tier that can
# succeed.
#
# Discovery order (tier 1 and 2 set spdk_linkable, tier 3 does not):
#   1. pkg-config (spdk_nvme.pc / spdk_env_dpdk.pc) — the packaging-correct
#      path once SPDK ships them. LINKABLE.
#   2. Sibling SPDK tree (../spdk, or -Dspdk_dir) with real built libraries
#      under build/lib. LINKABLE. An internal dev-rig convenience — mirrors
#      sdc/build_all.sh's SPDK_DIR — and explicitly NOT a packaging
#      contract (plan §8 design rule).
#   3. Sibling SPDK tree headers only, no library anywhere. NOT linkable:
#      compile-check target only.
#
# `spdk_backend` (feature: auto/enabled/disabled) gates this whole block so
# a tarball consumer can force SPDK discovery off deterministically
# (disabled), force it on and fail loudly if missing (enabled), or keep the
# current best-effort behavior (auto, the default).
spdk_backend_opt = get_option('spdk_backend')

spdk_sibling_dir = get_option('spdk_dir') != '' \
  ? get_option('spdk_dir') \
  : meson.project_source_root() / '..' / 'spdk'

# Default both signals to "no": every branch below either leaves them alone
# (headers-only / not found) or promotes them together.
spdk_linkable = false
spdk_link_dep = dependency('', required: false)

if spdk_backend_opt.disabled()
  spdk_dep = dependency('', required: false)
  message('SPDK backend discovery disabled via -Dspdk_backend=disabled')
else
  spdk_nvme_pc_dep = dependency('spdk_nvme', required: false)
  spdk_env_pc_dep = dependency('spdk_env_dpdk', required: false)

  if spdk_nvme_pc_dep.found() and spdk_env_pc_dep.found()
    # Tier 1: linkable. .pc files carry Libs/Requires, so this is the one
    # tier that can satisfy a shared-library link without extra help.
    spdk_dep = declare_dependency(dependencies: [spdk_nvme_pc_dep, spdk_env_pc_dep, thread_dep])
    spdk_link_dep = spdk_dep
    spdk_linkable = true
    message('Using SPDK via pkg-config (spdk_nvme.pc / spdk_env_dpdk.pc)')
  else
    spdk_sibling_hdr = spdk_sibling_dir / 'include' / 'spdk' / 'nvme.h'
    spdk_sibling_lib_dir = spdk_sibling_dir / 'build' / 'lib'

    spdk_sibling_lib_present = false
    foreach libname : ['libspdk_nvme.a', 'libspdk_nvme.so', 'libspdk_nvme.dylib']
      if run_command('test', '-f', spdk_sibling_lib_dir / libname, check: false).returncode() == 0
        spdk_sibling_lib_present = true
      endif
    endforeach

    if spdk_sibling_lib_present
      # Tier 2: real built libraries in the sibling tree (e.g. the .24/.28
      # dev rigs, which build SPDK from source without installing system
      # .pc files). Strict superset of the header-only case below, so
      # prefer it. Treated as linkable.
      #
      # NOTE for the Linux-rig bring-up task: SPDK ships static archives
      # with constructor-registered transports, so a real link of libcs
      # against this tier will additionally need the plan's §8 link
      # wrapping (-Wl,--whole-archive … -Wl,--no-whole-archive,
      # -Wl,--no-as-needed) plus DPDK's transitive deps (-luuid -lnuma -ldl
      # -lrt). Those flags are deliberately NOT added here: they cannot be
      # exercised, let alone verified, on this macOS dev box, and shipping
      # unverified linker flags is worse than shipping none. Tier 1
      # (pkg-config) does not need them.
      spdk_nvme_lib = cc.find_library('spdk_nvme', dirs: [spdk_sibling_lib_dir], required: false)
      spdk_env_lib = cc.find_library('spdk_env_dpdk', dirs: [spdk_sibling_lib_dir], required: false)
      if spdk_nvme_lib.found() and spdk_env_lib.found()
        spdk_dep = declare_dependency(
          compile_args: ['-I' + spdk_sibling_dir / 'include'],
          dependencies: [spdk_nvme_lib, spdk_env_lib, thread_dep],
        )
        spdk_link_dep = spdk_dep
        spdk_linkable = true
        message('Using SPDK libraries from sibling tree: ' + spdk_sibling_lib_dir)
      else
        spdk_dep = dependency('', required: false)
      endif
    elif run_command('test', '-f', spdk_sibling_hdr, check: false).returncode() == 0
      # Tier 3: headers only. spdk_dep.found() is true but there is NO
      # library to link, so spdk_linkable stays false and the real libcs
      # library must not pull in the SPDK-backed backend sources. This is
      # the only tier that can succeed on macOS.
      spdk_dep = declare_dependency(
        compile_args: ['-I' + spdk_sibling_dir / 'include'],
        dependencies: [thread_dep],
      )
      message('Using SPDK headers from sibling tree: ' + spdk_sibling_dir
              + ' (compile-check only; not linkable, so the SPDK backend is NOT built into libcs)')
    else
      spdk_dep = dependency('', required: false)
    endif
  endif

  # NB: deliberately gated on spdk_dep (discovery), not spdk_linkable —
  # `spdk_backend` is documented as controlling *discovery*, and tightening
  # it to require a linkable library is a separate decision from this
  # commit's headers-vs-linkable split.
  if spdk_backend_opt.enabled() and not spdk_dep.found()
    error('spdk_backend=enabled but SPDK could not be found via pkg-config ' +
          '(spdk_nvme.pc / spdk_env_dpdk.pc) or sibling tree (' + spdk_sibling_dir + ').')
  endif
endif

# Hardware backend gate. Formerly -DWITH_XNVME_BACKEND keyed off
# xnvme_dep.found(); the CPCS backend now rides directly on SPDK, so it
# keys off spdk_linkable (NOT spdk_dep.found() — see the long note above).
# The guarded C identifiers are unchanged: cs_backend_xnvme_register() et al
# keep their names until the mechanical rename in commit 9.
if spdk_linkable
  add_project_arguments('-DWITH_CPCS_BACKEND', language: 'c')
elif not get_option('sim_backend')
  error('No usable backend found. Build SPDK (install spdk_nvme.pc / ' +
        'spdk_env_dpdk.pc, or build a sibling ../spdk so that ' +
        'build/lib/libspdk_nvme.* exists), or enable sim_backend.')
endif

# Include directories
inc_dirs = include_directories('include', 'src')

# Library sources
lib_sources = [
  'src/cs_api.c',
  'src/cs_api_device.c',
  'src/cs_api_program.c',
  'src/cs_api_compute.c',
  'src/cs_helpers.c',
  'src/cs_helpers_cached.c',
  'src/backend/cs_backend_helpers.c',
]

# CPCS hardware backend: the 4 backend_xnvme*.c files (ported onto the
# libcs-native NVMe shim in commit 5 — filenames unchanged until commit 9)
# plus the shim itself (cs_nvme_cmds.c = CP/SLM command builders,
# cs_nvme_spdk.c = the SPDK transport). These are a single unit: the
# backend files call shim entry points that only cs_nvme_spdk.c defines,
# and cs_nvme_spdk.c calls SPDK. Hence the spdk_linkable gate.
if spdk_linkable
  lib_sources += [
    'src/backend/cs_nvme_cmds.c',
    'src/backend/cs_nvme_spdk.c',
    'src/backend/backend_xnvme.c',
    'src/backend/backend_xnvme_slm.c',
    'src/backend/backend_xnvme_compute.c',
    'src/backend/backend_xnvme_misc.c',
  ]
endif

if get_option('sim_backend')
  lib_sources += ['src/backend/backend_sim.c']
endif

if get_option('ioctl_backend')
  lib_sources += ['src/backend/backend_ioctl.c']
endif

liburing_dep = dependency('', required: false)
if get_option('io_uring_backend')
  liburing_dep = dependency('liburing', required: true,
    not_found_message: 'io_uring_backend requires liburing (apt install liburing-dev)')
  lib_sources += ['src/backend/backend_io_uring.c']
endif

libnvme_dep = dependency('', required: false)
if get_option('libnvme_backend')
  libnvme_dep = dependency('libnvme', required: true,
    not_found_message: 'libnvme_backend requires libnvme (apt install libnvme-dev)')
  lib_sources += ['src/backend/backend_libnvme.c']
endif

# Public headers
public_headers = [
  'include/cs_api.h',
  'include/cs_backend.h',
  'include/cs_helpers.h',
]

# Build the library.
#
# xnvme_dep is deliberately absent here: as of commit 6/10 libcs links no
# xNVMe. Only spdk_link_dep (tier 1/2, i.e. an actual library) may be added
# — spdk_dep may be nothing but header -I flags, which would let the static
# archive build and then fail the shared-library link with 21 undefined
# spdk_* symbols.
libcs_deps = [thread_dep, math_dep]
if spdk_linkable
  libcs_deps += [spdk_link_dep]
endif
if liburing_dep.found()
  libcs_deps += [liburing_dep]
endif
if libnvme_dep.found()
  libcs_deps += [libnvme_dep]
endif

libcs = library(
  'cs',
  lib_sources,
  include_directories: inc_dirs,
  dependencies: libcs_deps,
  install: true,
  version: meson.project_version(),
  soversion: version_major,
)

# Install headers
install_headers(public_headers, subdir: 'cs')

# Create dependency object for subproject usage
libcs_dep = declare_dependency(
  include_directories: include_directories('include'),
  link_with: libcs,
)

# Generate pkg-config file
pconf.generate(
  libcs,
  name: 'libcs',
  description: 'SNIA Computational Storage API Host Library',
  version: meson.project_version(),
  filebase: 'libcs',
  subdirs: ['cs'],
)

# Tests
test_sources = [
  'tests/test_cs_api.c',
]

test_exe = executable(
  'test_cs_api',
  test_sources,
  include_directories: include_directories('include', 'src'),
  link_with: libcs,
  dependencies: [thread_dep],
  install: false,
)

test('cs_api_tests', test_exe,
  env: [
    'LIBCS_DEBUG=1',
  ],
  timeout: 60,
)

# FILTER_GT focused tests
test_filter_exe = executable(
  'test_filter_gt',
  'tests/test_filter_gt.c',
  include_directories: include_directories('include'),
  link_with: libcs,
  dependencies: [thread_dep, math_dep],
  install: false,
)

test('filter_gt_tests', test_filter_exe,
  env: [
    'LIBCS_DEBUG=1',
  ],
  timeout: 120,
)

# Backend vtable completeness test (no device required)
test_vtable_exe = executable(
  'test_backend_vtable',
  'tests/test_backend_vtable.c',
  include_directories: include_directories('include', 'src'),
  link_with: libcs,
  dependencies: [thread_dep, math_dep],
  install: false,
)

test('backend_vtable_tests', test_vtable_exe,
  env: [
    'LIBCS_DEBUG=1',
  ],
  timeout: 60,
)

# Pure-logic unit tests (no device required, runs in CI)
test_logic_exe = executable(
  'test_xnvme_logic',
  'tests/test_xnvme_logic.c',
  include_directories: include_directories('include', 'src'),
  link_with: libcs,
  dependencies: [thread_dep, math_dep],
  install: false,
)

test('xnvme_logic_tests', test_logic_exe,
  env: [
    'LIBCS_DEBUG=1',
  ],
  timeout: 60,
)

# Integration tests (built but not run in CI — require a device)
test_backend_compute_exe = executable(
  'test_backend_compute',
  'tests/test_backend_compute.c',
  include_directories: include_directories('include', 'src'),
  link_with: libcs,
  dependencies: [thread_dep, math_dep],
  install: false,
)

# (tests/test_libcs_direct.c was deleted in commit 6/10: it was a raw-xNVMe
# baseline that #include'd <libxnvme.h> and called the fork's builders
# directly, not a test of libcs. It was built but never registered as a
# test(), and the golden-vector coverage in test_cmd_oracle.c supersedes it.)

# CP/SLM command-builder differential oracle (dev-only; needs xNVMe headers
# for the opcode/CSI enum cross-check, no device required). See
# tests/test_cmd_oracle.c for the full commit-sequence context.
#
# Commit 2 adds src/backend/cs_nvme_cmds.c (the libcs-native CP/SLM command
# builders) plus a test-only stub (tests/cs_nvme_spdk_test_stub.c) providing
# the handful of transport symbols cs_nvme_cmds.c's public wrappers
# reference but that aren't implemented until commit 3 (cs_nvme_spdk.c);
# see that stub's file header. Neither cs_nvme_cmds.c nor the stub needs
# xNVMe/SPDK to compile -- only the enum cross-check in test_cmd_oracle.c
# itself does -- so this executable is still gated on xnvme_dep.found() as a
# whole, kept simple rather than split into two differently-gated binaries;
# that gate goes away in commit 6 when the xNVMe dependency is dropped and
# commit 10 deletes this oracle outright.
if xnvme_dep.found()
  test_cmd_oracle_exe = executable(
    'test_cmd_oracle',
    [
      'tests/test_cmd_oracle.c',
      'tests/cs_nvme_spdk_test_stub.c',
      'src/backend/cs_nvme_cmds.c',
    ],
    include_directories: include_directories('include', 'src', 'src/backend'),
    dependencies: [thread_dep, xnvme_dep],
    install: false,
  )

  test('cmd_oracle_tests', test_cmd_oracle_exe,
    timeout: 60,
  )
endif

# Integration test (built but not run in CI — requires a live target URI,
# same pattern as test_backend_compute above)
test_compute_from_nvm_exe = executable(
  'test_compute_from_nvm',
  'tests/test_compute_from_nvm.c',
  include_directories: include_directories('include'),
  link_with: libcs,
  dependencies: [thread_dep, math_dep],
  install: false,
)


# SPDK transport compile-only proof (see
# ~/.claude/plans/hidden-sprouting-turtle.md). Compiles cs_nvme_spdk.c +
# cs_nvme_cmds.c against REAL SPDK headers -- not xNVMe's -- to catch any
# signature/layout mismatch as early as possible. static_library() rather
# than executable(): archiving never resolves undefined symbols, so this
# target only needs to prove the translation units compile, which is
# exactly why it can stay gated on spdk_dep (headers, any tier) while the
# real library is gated on spdk_linkable. Where SPDK *is* linkable these
# two sources are also in lib_sources; the duplicate compile is cheap and
# keeps the check meaningful on dev boxes with headers but no built SPDK.
# A no-op (not an error) wherever SPDK isn't available, or when
# -Dspdk_backend=disabled, per spdk_dep.found() below.
#
# On this macOS dev machine: no system pkg-config binary is installed and
# the sibling ../spdk tree has no build/ directory, so spdk_dep resolves
# via the header-only sibling fallback. Contrary to an earlier assumption
# recorded here, this fork's SPDK headers DO parse cleanly under Apple
# clang for this translation unit, so the compile-check actually builds
# (verified), it does not merely get skipped.
if spdk_dep.found()
  cs_nvme_spdk_compile_check = static_library(
    'cs_nvme_spdk_compile_check',
    [
      'src/backend/cs_nvme_spdk.c',
      'src/backend/cs_nvme_cmds.c',
    ],
    include_directories: include_directories('include', 'src', 'src/backend'),
    dependencies: [spdk_dep],
    build_by_default: true,
  )
endif

if get_option('sim_backend')
  subdir('benchmarks')
  subdir('examples')
endif

# Summary
summary({
  'Version': meson.project_version(),
  'Build type': get_option('buildtype'),
  'CPCS backend (SPDK, linked into libcs)': spdk_linkable,
  'SPDK headers found (compile-check target)': spdk_dep.found(),
  'xNVMe (dev-only, oracle test; dropped in commit 10)': xnvme_dep.found(),
  'Simulator backend': get_option('sim_backend'),
  'ioctl backend': get_option('ioctl_backend'),
  'io_uring backend': get_option('io_uring_backend'),
  'libnvme backend': get_option('libnvme_backend'),
}, section: 'Configuration')
