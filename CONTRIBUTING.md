# Contributing to libcs

Thank you for your interest in contributing to libcs! This document describes
how to set up a development environment and submit contributions.

## Quick Start

```bash
# Clone (or your fork)
git clone https://github.com/<your-github>/libcs.git
cd libcs

# Install build dependencies
pip install meson ninja

# Build with simulator backend (no hardware required)
meson setup build -Dsim_backend=true
meson compile -C build

# Run tests
meson test -C build -v
```

## Development Workflow

1. **Fork** the repository on GitHub
2. **Create a branch** from `main` for your change
3. **Build and test** locally (see Quick Start above)
4. **Commit** with a clear message and DCO sign-off (see below)
5. **Open a pull request** against `main`
6. **Address review feedback** — a maintainer will review your PR

## Code Style

- **Language**: C11 (GNU dialect, `gnu11`)
- **Indentation**: Tabs
- **Naming**: `snake_case` for functions and variables, `UPPER_CASE` for macros
- **Public API**: Functions prefixed with `cs` (e.g., `csOpenCSx`, `csAllocMem`)
- **Headers**: Use `extern "C"` guards in all public headers
- **Documentation**: Doxygen-style `/** */` comments on all public declarations
- **License header**: SPDX identifier at the top of every source file:

```c
// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause
```

## Build Configuration

libcs uses Meson. Key build options:

| Option | Default | Description |
|--------|---------|-------------|
| `sim_backend` | `true` | In-process simulator (no hardware needed) |
| `ioctl_backend` | `false` | Linux NVMe ioctl passthrough |
| `io_uring_backend` | `false` | Linux io_uring async passthrough |
| `libnvme_backend` | `false` | libnvme passthrough (NVMe-oF) |

```bash
# Debug build
meson setup build --buildtype=debug -Dsim_backend=true

# Build with xNVMe (if installed or in parent directory)
meson setup build

# Clean rebuild
meson setup build --wipe
```

## Testing

All tests run against the simulator backend — no NVMe hardware is required.

```bash
# Run all tests
meson test -C build

# Run a specific test
./build/test_cs_api

# Run with verbose output
meson test -C build -v
```

When adding a new feature, add a test case to `tests/test_cs_api.c` or create
a new test binary in `tests/`.

## DCO (Developer Certificate of Origin)

We use the Developer Certificate of Origin (DCO) instead of a Contributor
License Agreement (CLA). Every commit must be signed off:

```bash
git commit --signoff  # or: git commit -s
```

This adds a `Signed-off-by:` line to your commit message, certifying that you
have the right to submit the work under the project's license.

The full DCO text is available at:
https://developercertificate.org/

## Pull Request Guidelines

- **One change per PR** — keep PRs focused on a single issue or feature
- **Tests required** — new features must include tests
- **No breaking API changes** without discussion — open an issue first
- **Update documentation** — if you change the API, update `README.md` and
  relevant Doxygen comments
- **CI must pass** — all GitHub Actions checks must be green

## Reporting Issues

- **Bugs**: Open a GitHub issue with build info, reproduction steps, and expected
  vs. actual behavior
- **Feature requests**: Open a GitHub issue describing the use case
- **Security vulnerabilities**: See `SECURITY.md` — do NOT open a public issue

## Adding a New Backend

libcs supports pluggable backends via the `cs_backend_ops_t` vtable (see
`include/cs_backend.h`). To add a new backend:

1. Implement all required operations in `cs_backend_ops_t`
2. Register via `cs_backend_register()` or `csRegisterPlugin()`
3. Add a build option in `meson_options.txt` if the backend has dependencies
4. Add the source file to `src/` and wire it in `meson.build`
5. Add tests using the simulator as a reference

See `src/backend_sim.c` for a complete reference implementation.
