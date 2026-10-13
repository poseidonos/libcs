# Release Notes — libcs `oss-clean` (2026-10-09)

First open-source snapshot of the SNIA Computational Storage API host
library. This release is a cleanup/refactor release on top of the
hardware-validated baseline; no wire-format or semantic changes are
intended, and hardware regression SUM64 = 2080 PASS is recorded for the
final tree (see §4).

## Breaking changes (major bump)

1. **Plugin API removal (D3)** — `csRegisterPlugin` and
   `csDeregisterPlugin` are removed. They were a duplicate of the
   `cs_backend_register()` / `cs_backend_unregister()` registry in
   `cs_backend.h` (both shared a single registry). Migrate by renaming
   call sites to the `cs_backend_*` equivalents.

## Documented, deferred (not in this release)

- **`cp_output_returns` gap (D1, option c)** — the ioctl, libnvme and
  io_uring backends set the flag but never honor it; only xnvme branches
  on it. No code change; see `docs/coverage-gaps.md` §3. Post-release
  options: honor consistently in all four backends, or drop the field.

## Structural changes (no public-API effect)

- `src/backend_xnvme.c` (3,626 lines) split into per-concern translation
  units plus an internal header.
- `src/cs_api.c` (2,753 lines) split into lifecycle / program / compute /
  batch-query units.
- `src/cs_helpers.c` (1,559 lines) split into three units.
- Logic duplicated across the ioctl/libnvme/io_uring backends
  (CSF list query, kernel interpretation, submit/reap/abort, memory
  helpers, nsid handling) extracted into shared backend helpers.
- Internal `Cpcs*`/`CPCS` identifiers renamed to `Cs*`/neutral wording
  (e.g. `CpcsDotProductHeader` → `CsDotProductHeader`); wire magic
  `0x43504353` is unchanged.
- Benchmarks: shared sim-benchmark fixtures unified into
  `benchmarks/bench_common.h`; test fixtures into `tests/test_util.h`.

## Verification

- Meson build + `meson test` pass locally (sim backend).
- Hardware regression on an NVMe-oF/TCP SPDK target with a remote
  initiator host: SUM64 = **2080 PASS** — wire behavior identical to the
  pre-refactor baseline.

## Known gaps

See `docs/coverage-gaps.md` for the no-caller API list and untested
areas. The three previously-untested no-caller APIs (`csGetCSFInfo`,
`csGetMemInfo`, `csRunComputeAsync`) now have dedicated tests; the
remaining items (batch/vtable contract depth, fault injection) are
deferred past this release.
