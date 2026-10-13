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

# xNVMe dependency — always use the sibling build (NOT system pkg-config)
# to avoid linking against an outdated system libxnvme.so that may lack
# recent symbols (e.g. xnvme_cmd_slm_read).

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

if xnvme_dep.found()
  add_project_arguments('-DWITH_XNVME_BACKEND', language: 'c')
elif not get_option('sim_backend')
  error('No usable backend found. Build xNVMe or enable sim_backend.')
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
  'src/cs_backend_helpers.c',
]

if xnvme_dep.found()
  lib_sources += [
    'src/backend_xnvme.c',
    'src/backend_xnvme_slm.c',
    'src/backend_xnvme_compute.c',
    'src/backend_xnvme_misc.c',
  ]
endif

if get_option('sim_backend')
  lib_sources += ['src/backend_sim.c']
endif

if get_option('ioctl_backend')
  lib_sources += ['src/backend_ioctl.c']
endif

liburing_dep = dependency('', required: false)
if get_option('io_uring_backend')
  liburing_dep = dependency('liburing', required: true,
    not_found_message: 'io_uring_backend requires liburing (apt install liburing-dev)')
  lib_sources += ['src/backend_io_uring.c']
endif

libnvme_dep = dependency('', required: false)
if get_option('libnvme_backend')
  libnvme_dep = dependency('libnvme', required: true,
    not_found_message: 'libnvme_backend requires libnvme (apt install libnvme-dev)')
  lib_sources += ['src/backend_libnvme.c']
endif

# Public headers
public_headers = [
  'include/cs_api.h',
  'include/cs_backend.h',
  'include/cs_helpers.h',
]

# Build the library
libcs_deps = xnvme_dep.found() ? [thread_dep, math_dep, xnvme_dep] : [thread_dep, math_dep]
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

if xnvme_dep.found()
  test_libcs_direct_exe = executable(
    'test_libcs_direct',
    'tests/test_libcs_direct.c',
    include_directories: include_directories('include'),
    dependencies: [thread_dep, xnvme_dep],
    install: false,
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


if get_option('sim_backend')
  subdir('benchmarks')
  subdir('examples')
endif

# Summary
summary({
  'Version': meson.project_version(),
  'Build type': get_option('buildtype'),
  'xNVMe backend': xnvme_dep.found(),
  'Simulator backend': get_option('sim_backend'),
  'ioctl backend': get_option('ioctl_backend'),
  'io_uring backend': get_option('io_uring_backend'),
  'libnvme backend': get_option('libnvme_backend'),
}, section: 'Configuration')
