# Direct-transfer measurements and validation

Measured on AMD Ryzen 7 3700X using loopback TCP, a 64 MiB Host-local Dataset,
GCC Release `-O3 -DNDEBUG`, LTO, native CPU tuning, strict no-exception mode,
FF fast runner and the dedicated FF libuv worker. No injected work delay.
Normal and fast runner builds are both tested; the performance numbers here
are from fast runner. No machine configuration or dependency revision changed.

Measurements were collected from an uncommitted implementation based on
`92008504dc38246b5bc0549a9f674e9ed610a7c2`. Exact measured source content hashes,
binary hashes, compiler, flags and dependency commits are in [metadata.json](metadata.json).
All 99 measured source files were compared against the workspace after collection.

## Latency

Order: Table → direct → direct → Table. Each block omits 8 warmup transfers
and measures 100 alternating transfers; each path has 200 observations.
Table is administrative reassignment with Host snapshot, set/ACK and post-ACK
observation. Direct is conditional transfer by the current exclusive holder.
Recipient fetch/claim is included explicitly. These are useful end-to-end
paths with different semantics and RPC counts, not an isolated microbenchmark.

| Path / measurement | p50 (µs) | p95 (µs) | p99 (µs) |
|---|---:|---:|---:|
| table / Installed response | 122.052 | 134.552 | 146.992 |
| table / recipient fetch/claim | 60.351 | 65.640 | 70.461 |
| table / recipient-ready, client component sum | 182.592 | 198.783 | 207.183 |
| table / recipient-ready, driver wall time | 251.903 | 272.934 | 280.824 |
| table / first version call | 44.361 | 50.411 | 53.940 |
| direct / Installed response | 38.240 | 43.211 | 50.601 |
| direct / recipient fetch/claim | 31.400 | 35.880 | 38.060 |
| direct / recipient-ready, client component sum | 67.941 | 79.191 | 84.951 |
| direct / recipient-ready, driver wall time | 141.921 | 161.892 | 175.732 |
| direct / first version call | 44.200 | 56.201 | 62.650 |

The component sum includes encoding, FF, RPC and recipient materialization in
both client durations, excluding the Python dispatch gap between them. Wall
time includes that gap and process scheduling, measured with one driver clock.
Table Installed additionally waits for its post-ACK current-state observation.
Percentiles of component sums are computed from each pair, not by summing
the separate percentile columns. With 200 samples, p99 is descriptive only.

Each measured Table handoff made four Table→Host RPCs (three for the mutation
and one for fetch). Direct made zero Table RPCs; transfer plus claim contacted
Host directly. Resource summaries remained equal. Each block then measured
100 warm version calls without changing directory counters.

| Warm version path | p50 (µs) | p95 (µs) | p99 (µs) |
|---|---:|---:|---:|
| table | 43.381 | 46.661 | 56.690 |
| direct | 42.311 | 53.561 | 63.011 |

## In-flight drain

Twenty transfers followed actual Host admission of a summary RPC. Every
transfer was observed Pending; no artificial delay was used. The source
business and transfer connections were independent. Host segment timestamps
come from the same monotonic clock, with no client/Host clock subtraction.

| Segment | p50 (µs) | p95 (µs) | p99 (µs) |
|---|---:|---:|---:|
| client transfer to Installed response | 2403.633 | 2558.324 | 2634.526 |
| Host Close to observed final drain | 2355.822 | 2506.794 | 2556.845 |
| Host drain observation to Install | 0.030 | 0.040 | 0.050 |
| Host Install to reply callback | 2.960 | 4.580 | 4.640 |
| recipient claim | 43.491 | 52.761 | 120.992 |
| first version call | 57.230 | 65.801 | 66.420 |

The very small drain-to-install interval brackets the in-memory grant swap.
Clock granularity and measurement overhead preclude an isolated instruction-cost
interpretation. The dominant Close-to-drain interval is remaining legitimate
business work plus completion dispatch. A 20-sample p99 is effectively the maximum.

## Verification

| Configuration | Passed tests |
|---|---:|
| clang-strict | 32/32 |
| asan | 11/11 |
| strict | 32/32 |
| offline | 25/25 |
| eh | 33/33 |
| fast-strict | 32/32 |
| relaxed | 32/32 |
| tsan | 11/11 |

Exception mode includes the throwing-allocation authority rollback test.
ASan+UBSan and TSan are separate Debug builds with an 11-test lifecycle/transfer
selection, not full sanitizer suites. Offline disables GIO discovery and UV.
The actual Table handler test checks failed retries after Unknown, matching
duplicate ACKs and clearing first-attempt definitive rejection. Full test logs
are in [tests/](tests/); [matrix.json](matrix.json) records exact commands and exit codes.

Both nightly TLA+ models completed. Authority explored 43,018 distinct states;
DirectTransfer explored 19,292. All seven mutants exited 12 with their required
invariant violations. [TLA logs and commands](tla/report.json) retain the evidence.
These are bounded safety models, not C++/authentication/recovery proofs.

Earlier failed fixtures, a registry compatibility regression, model grouping
errors and review findings are recorded in [development-findings.json](development-findings.json).
The final implementation passes the checks above. Debug no-exception builds
retain an existing `-Wreturn-type` warning in unchanged `async/detachable.hpp`;
the compiler logs are retained under `builds/`. The task package and initial
working-tree changes were preserved. These local measurements predate the final publication review and the narrated
demo. Their hashes and counts identify that measured snapshot; current CI validates
the published revision separately.

## Reproduce

```sh
cmake -S . -B build-fast-strict -DCMAKE_BUILD_TYPE=Release \
  -DCAPSTAN_NO_EXCEPTIONS=ON -DCAPSTAN_NO_EXCEPTION_STRICT=ON \
  -DCAPSTAN_FAST_RUNNER=ON -DCAPSTAN_UV_WORKER=ON \
  -DCAPSTAN_LTO=ON -DCAPSTAN_NATIVE_ARCH=ON
cmake --build build-fast-strict --parallel 8
ctest --test-dir build-fast-strict --output-on-failure
python3 scripts/bench_direct_transfer.py --build-dir build-fast-strict \
  --output benchmark-output --samples 100
python3 scripts/check_tla.py --jar /path/to/tla2tools.jar --nightly
```

Raw paired samples, all Host timestamps, process commands and logs are under
[benchmark/](benchmark/report.json). Credentials were temporary private files
and were removed. Persistent services are supervisor-terminated; those exits
are explicitly recorded, not claimed as natural zero exits. This is one-machine
Python-paced loopback evidence, with logging enabled; there is no saturation,
cross-machine, HA or hostile-network claim.

## Publication review

[The final review record](publication-review.json) covers the additional demo and
Completed-envelope validation added after these measurements: EH 34/34, strict,
relaxed, fast-strict and Clang strict 33/33 each, offline 25/25, and separate
ASan+UBSan/TSan selections 12/12 each. Both nightly models and all seven mutations
were checked again. The earlier benchmark source hashes and samples remain
unchanged; publication review is separate validation, not a new timing run.
