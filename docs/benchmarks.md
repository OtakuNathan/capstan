# Latest loopback measurements

Measured on 2026-11-04, AMD Ryzen 7 3700X (8 cores / 16 threads), Linux x86_64,
GCC 16.2.1, GIO 2.90.1 and libuv 1.53.0. The CPU power policy was not changed.
These are local multiprocess TCP experiments, not cross-machine results.

All comparison binaries use the exact published upstream commits in
`deps.lock.json`, C++14, `-O3 -g -DNDEBUG`, LTO, native CPU tuning, GIO I/O,
a dedicated UV business worker, the normal FF runner, the same codec and full
protocol/Lease/serial-lane semantics. There are no requested cancellations.
The 64 MiB Dataset stays on Host; summary returns a small result.

## Exception policy comparison

Each mode occupies each order position once over three rotated rounds. Per mode:
6,000 version samples, 300 summary samples and 300 in-flight authority handoffs.
All 900 handoffs observed pending work. Each transition uses one control RPC;
Host pins drain before install. Results match, each run ends with admitted work
completed and active=0, and warm business calls do not access Table.

| Operation | Mode | p50 | p95 | p99 |
|---|---|---:|---:|---:|
| version | eh | 39.200 µs | 43.339 µs | 48.110 µs |
| version | strict | 39.220 µs | 43.840 µs | 49.100 µs |
| version | relaxed | 38.860 µs | 43.850 µs | 51.660 µs |
| summary | eh | 2.466 ms | 2.485 ms | 2.562 ms |
| summary | strict | 2.457 ms | 2.509 ms | 2.572 ms |
| summary | relaxed | 2.447 ms | 2.508 ms | 2.547 ms |
| handoff | eh | 2.465 ms | 2.489 ms | 2.511 ms |
| handoff | strict | 2.453 ms | 2.482 ms | 2.535 ms |
| handoff | relaxed | 2.442 ms | 2.475 ms | 2.507 ms |

No clear end-to-end speedup from disabling exceptions was observed. The Host
`.text` section is 126,961 bytes with exceptions and 102,223 bytes in both
no-exception modes (19.5% smaller). `.gcc_except_table` is 3,379 bytes in EH and
absent in the no-exception binaries. This is code-section size, not a claim about
RSS or a debug-symbol-inflated executable size. Unwind information may remain
for stack traces. Strict/relaxed policies change accepted declarations, not
necessarily generated instructions.

## Host grant-validation increment

A separate four-round AB/BA run uses the same strict no-exception benchmark Host
binary. The only changed switch skips Host grant/effect admission validation.
Transport, codec, FF path, authentication envelope, Lease, operation pins, serial
lane, cancellation capability and business operation are identical. Production
`capstan_host` does not expose this bypass.

| Warm version | Samples | p50 | p95 | p99 |
|---|---:|---:|---:|---:|
| omit | 4000 | 40.050 µs | 44.790 µs | 51.900 µs |
| checked | 4000 | 41.080 µs | 45.500 µs | 58.100 µs |
| Quantile difference | — | +1.030 µs | +0.710 µs | +6.200 µs |

These are differences between sampled latency quantiles, not paired CPU timings
or an isolated instruction cost. Scheduling and logging can dominate this small
increment; no statistical-significance claim is made. This comparison does not
measure the whole capability runtime against a raw socket implementation.

The raw report also contains serial shared readers, quiescent handoff at varying
write intervals, and overload/backpressure probes. Quiescent handoff groups have
only 2–4 samples and must not be treated as reliable p99 estimates. Overload
samples mix successful calls and local rejection; the overload workload injects
5 ms business delay and is not comparable to the main latency table. RSS is a
10 ms observed sample, not a proof of a global memory bound. Python/stdin pacing
and stdout logging remain enabled; this is not a saturation-throughput benchmark.

## Reproduce and inspect

Build `build-eh`, `build-strict` and `build-relaxed` with the flags above and the
corresponding exception options. For the exact measured release flags, pass
`-DCMAKE_CXX_FLAGS_RELEASE="-O3 -g -DNDEBUG"` in addition to the options in
[build and validation](build-and-validation.md).

```sh
python3 scripts/run_noexceptions_comparison.py   --eh build-eh --strict build-strict --relaxed build-relaxed   --output build-measurements/exceptions
python3 scripts/run_benchmark.py --build-dir build-strict   --samples 1000 --rounds 4 --output build-measurements/cap-increment
```

Latest published artifacts:

- [Exception raw samples](../bench/latest/exceptions/raw.csv) and [report](../bench/latest/exceptions/report.json).
- [Grant-validation raw samples](../bench/latest/cap-increment/raw.csv) and [report](../bench/latest/cap-increment/report.json).
- [Environment](../bench/latest/environment.json), [source fingerprints](../bench/latest/source-manifest.json), and [verification results](../bench/latest/verification.json).
- `bench/latest/*-size.txt` contains `size -A` output; `bench/latest/tla/` contains
  the safe-model result and three expected counterexample logs.

Reports preserve flags, sample counts, binary SHA256 fingerprints and exact
dependency commits. Verification commands normalize the working directory to the
repository root; TLA log paths are similarly normalized. Session credentials and
private machine configuration are not included. Optimization history and older
benchmark runs are not part of this repository.

## Holder-transfer follow-up

The measurements above describe the published pre-direct-transfer baseline.
[Latest direct-transfer measurements](../bench/direct-transfer/latest/README.md)
compare the updated Table snapshot path with conditional holder transfer, including
recipient claim. The paths have different administrative semantics and RPC counts;
those numbers must not be treated as an isolated instruction-cost comparison.
