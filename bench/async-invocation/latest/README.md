# Owned async invocation comparison — 2026-11-05

The concrete callable path saves one counted allocation relative to type erasure
in each microbenchmark. This experiment does **not** establish a consistent
end-to-end throughput improvement over Capstan's previous C-style adapter.

## Machine and scope

AMD Ryzen 7 3700X, 8 cores / 16 threads; Linux x86_64; GCC 16.2.1;
CPU governor `powersave`. All TCP builds use Release (`-O3 -DNDEBUG`), C++14,
strict no-exceptions, FF fast runner and the libuv worker. LTO and native CPU
tuning are **off in every comparison build**. These are matched refactor
measurements, not the earlier maximum-tuning results.

Baseline is Capstan `1758a164b6ae8e729132038d3c5d342186f36056` with its locked
component versions. New static and erased binaries use implementation commit
`8c2ce13` and the exact component revisions in [metadata.json](metadata.json).
The erased variant is a full build, including Host, Table, client and driver.
No build, test or sanitizer job launched for this task overlaps a timed batch.
Background OS activity, dynamic frequency and thermal behavior are not controlled.

TCP measurements use one 64 MiB object, loopback TCP, one outstanding request
per connection, inline scalar execution, tracing off and a finite 65,536-entry
history. Each batch validates results and drained Host accounting. They do not
measure open-loop arrivals, cross-machine networking, multiple objects or
indefinite-service capacity.

## Interleaved scalar RPC confirmation

Three 50,000-call batches per mode and connection count, with 128 warmups each.
Mode orders are baseline/static/erased, erased/static/baseline, then
static/baseline/erased. Connection order within each batch is 1, 4, 16.
Latency below is the median of the three batch p99 values, not a pooled percentile.

| Mode | Connections | RPC/s median | RPC/s min–max | Batch p99 median µs |
| --- | ---: | ---: | ---: | ---: |
| baseline | 1 | 33,548 | 31,398–34,117 | 32.93 |
| static | 1 | 33,682 | 33,191–34,100 | 32.73 |
| erased | 1 | 33,236 | 33,018–34,058 | 33.16 |
| baseline | 4 | 63,815 | 62,470–71,186 | 74.08 |
| static | 4 | 68,922 | 65,480–71,328 | 70.22 |
| erased | 4 | 68,049 | 66,832–71,760 | 67.99 |
| baseline | 16 | 60,950 | 60,920–61,563 | 277.57 |
| static | 16 | 55,741 | 55,536–63,302 | 300.60 |
| erased | 16 | 59,395 | 59,097–63,866 | 325.68 |

Relative to baseline, concrete calls are approximately +0.4%, +8.0%, and -8.5%
at 1, 4, and 16 connections in this confirmation. The differing directions and
batch ranges prevent a general speedup claim. The initial sequential run used
30,000 scalar calls per batch and showed a roughly 6% single-connection decrease;
its complete outcomes remain in `baseline.json`, `static.json`, and `erased.json`.
The follow-up does not erase that observation. A stable performance regression
would need a longer experiment with controlled CPU placement/frequency.

## 64 MiB summary RPC

Three sequential batches per mode, 512 measured calls and 128 warmups each.
Latency is the pooled p99 within each mode/connection group from the original
reports. This scan is dominated by Dataset traversal and memory traffic.

| Mode | Connections | RPC/s median | RPC/s min–max | Pooled p99 µs |
| --- | ---: | ---: | ---: | ---: |
| baseline | 1 | 309 | 305–312 | 3428.93 |
| static | 1 | 306 | 300–306 | 3403.80 |
| erased | 1 | 307 | 303–321 | 3415.20 |
| baseline | 4 | 318 | 309–325 | 13044.40 |
| static | 4 | 313 | 312–316 | 13051.96 |
| erased | 4 | 314 | 309–322 | 13060.28 |
| baseline | 16 | 309 | 308–331 | 52920.74 |
| static | 16 | 313 | 307–330 | 52306.16 |
| erased | 16 | 312 | 310–313 | 52339.10 |

## No-I/O paired benchmark

`dynabridge/benchmarks/async_invocation.cpp`, C++14, `-O3 -DNDEBUG -pthread`.
Three runs, reversing static/erased order in the middle run. The table reports
median ns/call. Counts intercept global `operator new`; they are not a census
of every allocator API. A context owner is prepared outside the timed loops.
The raw runs and compiler command are included in this directory.

| Scenario | Concrete ns/call | Erased ns/call | `new` calls: concrete / erased |
| --- | ---: | ---: | ---: |
| discard | 5.29 | 15.89 | 0 / 1 |
| inline | 62.05 | 101.72 | 1 / 2 |
| ff_inline | 144.73 | 193.74 | 2 / 3 |
| move_only_64k | 1258.95 | 1176.81 | 2 / 3 |
| thread_create_join | 22072.19 | 22567.89 | 2 / 3 |

The 64 KiB case includes payload allocation and initialization, and its result
cannot be default-constructed or copied. The thread case includes creating and
joining a thread for each call; it is not pure scheduler handoff latency.
Small differences in these dominated cases should not be attributed to erasure.

## Validation

- Dynabridge: 24/24 runtime tests and 18 async strict/relaxed configure probes.
- Capstan: 38/38 full tests in the concrete build and 38/38 in the erased build.
- Desktop ASan+UBSan: async contracts, typed Capstan calls, and callback regressions.
- Desktop TSan: async completion/detach races and retained callback state.
- Exception-enabled allocation fault sweep, Clang strict/erased typed calls,
  and portable CUDA completion-condition tests all passed.
- Existing TLA+ authority models: 9/9 expected outcomes, including seven deliberate
  counterexamples. These models do not prove C++ ownership or GPU completion.
- CUDA typed demo was **not compiled or run during the measurements above**.
  The subsequent review installed CUDA and ran all eight typed GPU tests on the
  RTX 2080 Ti without skips. See [review validation](review.json) and the
  [FF-owned demo walkthrough](https://github.com/OtakuNathan/flux_foundry/blob/a18b05a464532d8c9acaa048e873884606a356f4/docs/typed_async_demo.md).
  The recorded CPU timings and their original component pins have not been relabeled.
- GCC/Clang erased configurations and the new lifecycle tests are added to CI.
  The original measurement run did not include a GitHub Actions result; CI
  outcomes are separate from these local benchmark records.

See [sanitizer and targeted checks](sanitizers-and-targeted-tests.json),
[formal outcomes](formal.json), and the saved CTest logs. JSON reports retain
individual batch statistics and source/binary fingerprints; individual RPC sample
files and temporary credentials are not included in this public report.

The default remains concrete callable storage. Select `--erased-invocations`
with `scripts/bench_machine.py` when a common `invocation<R>` boundary is useful.
That convenience has a measured local cost; neither option changes authority,
timeout/Unknown, late completion, or Drain semantics.
