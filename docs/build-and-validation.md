# Builds, contracts and CI

Use CMake 3.20+, Python 3.12+, Git and a C++14 compiler. Linux/macOS TCP requires
pkg-config and GIO; the optional UV worker also requires libuv development files.
`python3 scripts/fetch_deps.py` explicitly fetches the SHA-pinned upstream commits.
CMake snapshots commits, not dirty working trees. A changed lock requires a fresh
build tree; existing dependency checkouts are never reset automatically.

## Optimized and no-exception builds

```sh
cmake -S . -B build-strict -DCMAKE_BUILD_TYPE=Release \
  -DCAPSTAN_UV_WORKER=ON -DCAPSTAN_LTO=ON -DCAPSTAN_NATIVE_ARCH=ON \
  -DCAPSTAN_NO_EXCEPTIONS=ON -DCAPSTAN_NO_EXCEPTION_STRICT=ON
cmake --build build-strict --parallel 2
ctest --test-dir build-strict --output-on-failure
```

Use `CAPSTAN_NO_EXCEPTION_STRICT=OFF` in a separate build for relaxed checks;
use `CAPSTAN_NO_EXCEPTIONS=OFF` for the exception-enabled comparison. The switch
maps to FF and Dynabridge's no-exception policies. Strict rejects selected
unannotated converter/storage construction extensions. Relaxed permits those
declarations; it does not enable exceptions, bypass authorization, or relax
Lease topology and FF task/awaitable's mandatory noexcept requirements.

Checked decode/dispatch, local Busy/duplicate creation and submission rejection
use explicit results in both modes. Legacy must-succeed wrappers throw with
exceptions enabled and assert/abort without them, including under NDEBUG.
Standard container/shared_ptr allocator exhaustion is process-fatal without
exceptions. A nothrow FF submission allocation failure remains NotSubmitted.
The UV checked constructor reports parameter/libuv setup errors but does not
promise recoverable standard-allocator OOM.

The exception-only exhaustive throwing-allocation rollback test is omitted in
no-exception builds. This is why the full Release suites contain 38 and 37 tests,
respectively; it is not counted as a passed no-exception OOM test. Backend-wide
Python/Node no-exception compatibility is not claimed.

`CAPSTAN_FAST_RUNNER=ON` removes runner cancellation and changes that contract;
benchmark it as a separately labelled configuration. Defaults retain the normal
runner. `CAPSTAN_NATIVE_ARCH` is intended for builds run on their build CPU.
GCC's `CAPSTAN_VECTORIZATION_REPORT=ON` emits optimization diagnostics.

## Verification layers

- Compile probes and negatives cover Lease copying/readonly access, typed receiver
  identity, generated-import const limitations and async return adaptation.
- The eight-case FF/Dynabridge compile-policy matrix requires appropriate strict
  failures and relaxed successes. Negative subprocesses inherit the build policy.
- C++ tests cover admission/transition ordering, stale authority, allocation
  rejection, explicit errors, operation retention and callback races.
- Real multiprocess TCP tests exercise shared/exclusive access, queue ordering,
  bad frames, stale/forged authority, timeout/disconnect, lost control sessions,
  and destruction after Drain. A fault probe sends bad requests and then checks
  that both services remain responsive.
- The Python model is a small bounded independent CTest check. The TLA+ model
  additionally models the control/ACK/publication stages and runs nightly.

```sh
python3 scripts/run_demo.py --build-dir build-strict
python3 scripts/run_direct_demo.py --build-dir build-strict
python3 scripts/run_fault_suite.py --build-dir build-strict
python3 scripts/run_ack_faults.py --build-dir build-strict --output build-strict/ack-faults
python3 scripts/check_tla.py --download-tools --nightly
```

Demo output includes process commands, logs and real exit codes. The supervisor
normally terminates persistent Host/Table processes with SIGTERM; deliberate
SIGKILL fault injection and cleanup are not presented as natural exit zero.
Private session credentials are created in temporary storage and not published.

For sanitizer builds, use Debug with either `CAPSTAN_ASAN=ON` (ASan+UBSan) or
`CAPSTAN_TSAN=ON`, never both. The CI lifecycle selection includes
`authority_replies_test`, `checked_errors_test`, `callback_test`, `allocation_test`,
`worker_test`, `tcp_ack_faults`, `direct_transfer_test`,
`direct_transfer_integration_test`, `sealed_registry_test`, `table_observation_test`,
`tcp_direct_transfer`, `direct_transfer_demo`, `transport_test`,
`host_execution_test`, `tcp_execution_modes`, and `tcp_throughput_smoke`.
The added checks cover idle established connections, write/read FIFO ordering
and the Drain barrier in both scalar placement modes, finite history across
handoffs, and validated native throughput batches. Full sanitizer coverage is
not claimed.

## CI baseline

`.github/workflows/ci.yml` builds optimized GCC/Clang × exception/strict/relaxed/
fast-strict configurations with UV, LTO and native CPU tuning. It runs the complete
CTest suite. A separate Debug/no-exception job disables pkg-config discovery and
verifies offline support. Separate ASan+UBSan and TSan jobs run the lifecycle/TCP
selection. Failures retain test artifacts. There are no flaky latency thresholds
or performance claims derived from shared GitHub runners.

An Apple Silicon `macos-15` job runs the complete fast/strict suite and the
machine benchmark's quick profile. It checks compatibility and result validation;
its small batches are not published as M1 Max performance measurements.

`.github/workflows/formal-nightly.yml` runs TLA+ at 02:23 UTC and on manual
request, using a hash-pinned jar. Both safe models must complete and each of seven
mutants must fail for its expected invariant. See [formal verification](formal-verification.md).

Latest actual local results, upstream revisions and tool versions are recorded
in `bench/latest/verification.json` and [the measurement report](benchmarks.md).
Windows Capstan TCP and cross-machine RPC are not validated. See the
[machine benchmark guide](benchmark-your-machine.md) for native macOS usage.

## Direct-transfer validation

The same CI matrix includes holder transfers and Table compatibility. Run
`python3 scripts/run_direct_transfer.py --build-dir build-strict --output build-strict/direct`
for the Table-pause and disconnect scenarios. The benchmark script
`bench_direct_transfer.py` uses ABBA ordering and reports Installed, recipient-ready
including claim, first-call and Host-local drain segments independently. Current
local results are in [the direct-transfer report](../bench/direct-transfer/latest/README.md).
The nightly workflow verifies that both safe models and all seven mutations ran,
and publishes their outcomes and state counts in the Actions summary.

`direct_transfer_demo` is also a CTest entry in every GIO-enabled full suite.
It checks actual business updates and a return transfer with Table stopped; see
[the walkthrough](direct-transfer-demo.md). The recorded benchmark matrix predates
this additional demo entry and retains its actual 33/32 test counts.
