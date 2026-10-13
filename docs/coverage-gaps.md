# Test Coverage Gaps

Status of library API test coverage as of the `oss-clean` refactoring
(2026-10-09). Regenerate the no-caller list with:

```sh
for fn in $(grep -oP '^\s*csStatus \Kcs\w+|^\s*const char \*\Kcs\w+|^\s*void \Kcs\w+|^\s*cs\w+ \Kcs\w+' include/cs_api.h include/cs_helpers.h | sort -u); do
  n=$(grep -rw "$fn" tests benchmarks examples --include='*.c' | wc -l)
  [ "$n" -eq 0 ] && echo "no-caller: $fn"
done
```

## 1. Public APIs with no caller in tests/benchmarks/examples

Update (2026-10-13): the three untested APIs below now have dedicated tests —
`csGetCSFInfo` / `csGetMemInfo` / `csRunComputeAsync` via sim-backend unit
tests in `tests/test_cs_api.c`.

| API | Internal use | Disposition |
|-----|--------------|-------------|
| `csGetCSFInfo` | none (definition only) | ✅ tested by `test_sim_get_csf_info` |
| `csGetMemInfo` | none (definition only) | ✅ tested by `test_sim_get_mem_info` |
| `csRunComputeAsync` | none (definition only) | ✅ tested by `test_sim_run_compute_async` |
| `csQueueCopyMemRequest` | called by `cs_api.c` batch dispatch and `cs_helpers.c` staging paths | exercised indirectly; add a direct unit test |
| `csQueueStorageRequest` | called by `cs_api.c` batch dispatch | exercised indirectly; add a direct unit test |
| `csRegisterPlugin` | `tests/test_cs_api.c` (registration bookkeeping test only) | removed by D3 (see below) |
| `csDeregisterPlugin` | `tests/test_cs_api.c` (same) | removed by D3 (see below) |

Deferred (post-release): add program-lifecycle-focused tests for the
remaining no-caller list above, prioritizing the lifecycle-heavy APIs
(load/activate/deactivate/download paths) over trivial getters.

## 2. Areas without dedicated tests

- **Batch API semantics** — `csAllocBatchRequest` … `csSubmitBatchRequest`
  are covered by smoke tests only; reconfigure/resize error paths are untested.
- **Backend vtable contract tests** — `test_backend_vtable.c` checks wiring,
  not per-op behavior contracts (e.g. `compute_submit` → `compute_reap`
  pairing, `abort_request` invariants).
- **Error injection** — no fault-injection harness for backend I/O failures
  (`CS_STATUS_ERROR_IO` propagation).

## 3. Known behavioral gap: `cp_output_returns` (D1, option c)

**Scope**: ioctl, libnvme and io_uring backends set the
`cp_output_returns` field in their compute command context but never honor
it; only the xnvme backend branches on it (4 sites). No wire-format or
semantic change is made in this release — the gap is documented only.

Effect today: for the three backends above, the field is dead state. Any
future consumer expecting controller→host inline output differentiation
must not rely on `cp_output_returns` outside the xnvme backend.

Planned resolution (post-release): either honor the flag consistently in
all four backends or drop the field. Decided as **(c) document-and-defer**
for this release to keep the snapshot wire-identical to the hardware
baseline (SUM64 = 2080, see the release notes).

## 4. Deprecated / removed in this release

- `csRegisterPlugin` / `csDeregisterPlugin` — removed (D3). Use
  `cs_backend_register()` / `cs_backend_unregister()` from `cs_backend.h`;
  both shared one registry, so no migration of call sites beyond renaming
  is required.
