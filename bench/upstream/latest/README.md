# Portal / Flux Foundry dependency comparison — 2026-11-18

No broad end-to-end speedup from dependency replacement alone. Primary scalar results vary by concurrency; four-core affinity confirmation is close to flat, with a small single-connection worker decrease. Some large Paste medians improve; Cut is essentially unchanged.

No Capstan implementation code or permission/Drain policy changed in this experiment. The repository lock remains unchanged; this report records the tested candidates.

## Revisions

Capstan source: `602ab1666c8a8b2784577d9b60410183568c894f`. Exact four-way locks: [variants.json](variants.json).

| Variant | Portal | Flux Foundry | Validation |
|---|---|---|---|
| baseline | `7a6f53bbc30d` | `2be3825e7958` | 56/56 CTest checks |
| portal | `7c0ac2fded1a` | `2be3825e7958` | 5/5 CTest checks |
| ff | `7a6f53bbc30d` | `030bd9e7c8e8` | 5/5 CTest checks |
| both | `7c0ac2fded1a` | `030bd9e7c8e8` | 56/56 CTest checks |

Baseline and both-updated builds ran the complete strict/no-exception Capstan suite (56 tests each). The single-library ablations ran async, worker, Host execution, no-exception policy and TCP throughput checks (five each). Every timed batch also validated all responses and drained Host accounting.

## Machine and method

Physical Ryzen 7 3700X, 8 cores / 16 threads, 32 GiB RAM; Linux 7.3.4-1-cachyos, GCC 16.2.1, GIO 2.90.1, libuv 1.53.0, OpenSSL 4.1.0. CPU governor: powersave. This is an active desktop, not an isolated benchmark host. No governor or system affinity settings were changed.

All variants use C++14, Release/O3, LTO, `-march=native`, strict no-exceptions, FF fast runner, concrete invocations, UV worker and OpenSSL SHA-256. Full flags and binary SHA256 values are in [environment.json](environment.json); commands and validation are in [validation.json](validation.json). Builds and correctness tests finished before timed runs.

One 64 MiB Dataset, loopback TCP, one outstanding RPC per connection, finite 65,536-request history, no tracing. Three interleaved batches per variant/point: 50,000 scalar calls plus 1,000 warmups; 512 summary calls plus eight warmups; 30,000 forced-worker scalar calls plus 128 warmups. Variant and concurrency order rotate between rounds. Total: 156 RPC batches.

Tables use median batch throughput and median of batch p99 values, not pooled percentiles. The JSON retains min/max rates, p50/p95/p99 and all per-batch results. Ranges are observed variation, not confidence intervals. Host and load-generator dependencies change together; this is an end-to-end comparison, not a server-only attribution.

## Default scalar path

| Connections | Baseline RPC/s | Portal only | FF only | Both | Both change | Baseline p99 µs | Both p99 µs |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 30,362 | 29,987 | 31,184 | 30,088 | -0.9% | 60.33 | 56.59 |
| 2 | 47,498 | 47,987 | 46,851 | 46,248 | -2.6% | 66.16 | 83.06 |
| 4 | 57,758 | 58,279 | 62,456 | 59,999 | +3.9% | 127.92 | 112.46 |
| 8 | 59,451 | 57,390 | 58,117 | 56,631 | -4.7% | 202.01 | 218.80 |
| 16 | 54,394 | 54,989 | 53,749 | 52,778 | -3.0% | 424.75 | 427.12 |
| 32 | 45,025 | 44,605 | 44,808 | 45,130 | +0.2% | 1002.49 | 1032.07 |
| 48 | 39,615 | 39,072 | 39,627 | 39,815 | +0.5% | 1597.62 | 1626.69 |

## Forced worker path

| Connections | Baseline RPC/s | Portal only | FF only | Both | Both change | Baseline p99 µs | Both p99 µs |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 24,647 | 23,728 | 23,804 | 23,398 | -5.1% | 73.63 | 96.30 |
| 8 | 47,672 | 50,316 | 50,424 | 49,493 | +3.8% | 256.07 | 232.97 |
| 32 | 42,348 | 43,128 | 39,282 | 42,862 | +1.2% | 1150.78 | 1027.23 |

## CPU-affinity confirmation

The primary eight-connection scalar decrease and one-connection worker decrease warranted a separate check. Four alternating batches per point/variant ran under `taskset -c 0-3`; children inherited four distinct physical cores sharing one L3/CCX. This affects only benchmark processes. These are separate measurements, not replacements for or pooled with the unrestricted 16-thread sweep. See [topology](confirmation-topology.txt).

| Path | Connections | Baseline RPC/s | Both RPC/s | Change | Baseline p99 µs | Both p99 µs |
|---|---:|---:|---:|---:|---:|---:|
| version | 8 | 60,973 | 60,852 | -0.2% | 174.62 | 176.80 |
| version-worker | 1 | 28,393 | 27,523 | -3.1% | 45.42 | 48.81 |
| version-worker | 8 | 57,096 | 56,788 | -0.5% | 190.36 | 194.41 |

The eight-connection differences nearly disappear under this affinity profile. Scheduling/cache placement is therefore a plausible contributor to the primary variation, not an established code cause. The one-connection worker median still decreases about 3%; that is a follow-up profiling signal, not evidence of a broad regression or speedup.

## Summary scans

| Connections | Baseline RPC/s | Portal only | FF only | Both | Both change |
|---:|---:|---:|---:|---:|---:|
| 1 | 376.1 | 373.6 | 375.7 | 367.8 | -2.2% |
| 8 | 374.3 | 393.6 | 383.4 | 379.4 | +1.4% |
| 32 | 373.9 | 390.4 | 381.6 | 377.0 | +0.8% |

## Copy/Paste and Cut

Baseline and both-updated builds use the same strict/LTO/native profile above. This differs from the older exception-enabled, erased-invocation migration table in the main README; compare only the matched rows here. Three alternating variant blocks, two measured runs per size/block, one discarded warmup per size/block: six measured samples per size/variant. These sequential end-to-end timings have no saturation or p99 interpretation.

| MiB | Baseline Cut ms | Both Cut ms | Change | Baseline Paste #1 ms | Both Paste #1 ms | Baseline Paste #2 ms | Both Paste #2 ms |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 4.06 | 4.46 | +10.1% | 3.70 | 3.46 | 2.51 | 2.26 |
| 8 | 19.06 | 19.12 | +0.3% | 14.77 | 13.31 | 13.53 | 13.20 |
| 32 | 71.03 | 73.18 | +3.0% | 53.99 | 46.93 | 49.39 | 45.53 |
| 64 | 136.14 | 137.07 | +0.7% | 99.15 | 92.84 | 102.89 | 90.85 |

At 64 MiB, Paste #1/#2 medians decrease about 6.4%/11.7%, while Cut changes only +0.7%. Some large-Paste improvement is visible in this run; no general migration speedup follows from it.

## Scope and reproduction

This experiment upgrades existing dynamic/framed RPC and FF integration headers. It does not switch Capstan to Portal direct typed RPC imports or runtime registration, or enable explicit FF identity/controller-pool policies. It does not measure GPU, persistent SQLite throughput, cross-machine traffic, open-loop arrivals, indefinite-service capacity or physical-power-loss recovery. Upstream microbenchmark improvements should not be read as automatic gains for this workload.

Use isolated source copies of the recorded Capstan commit and the exact locks in `variants.json`. Configure each against its own locked dependency export with these common options:

```sh
cmake -S SOURCE -B BUILD -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ \
  -DCAPSTAN_SQLITE=ON -DCAPSTAN_REQUIRE_GIO=ON -DCAPSTAN_SHA256_BACKEND=OPENSSL \
  -DCAPSTAN_NO_EXCEPTIONS=ON -DCAPSTAN_NO_EXCEPTION_STRICT=ON \
  -DCAPSTAN_FAST_RUNNER=ON -DCAPSTAN_UV_WORKER=ON \
  -DCAPSTAN_ERASED_INVOCATIONS=OFF -DCAPSTAN_LTO=ON -DCAPSTAN_NATIVE_ARCH=ON
cmake --build BUILD --parallel 3
ctest --test-dir BUILD --output-on-failure --timeout 90
python3 SOURCE/scripts/bench_concurrency.py --build-dir BUILD --output OUTPUT \
  --workloads version --concurrency 8 --rounds 1 --samples 50000 --warmup 1000 \
  --history-limit 65536 --inline-version 1 --trace-requests 0 --resource-bytes 67108864
```

Supply `CAPSTAN_LEASE_SOURCE`, `CAPSTAN_PORTAL_SOURCE` and `CAPSTAN_FF_SOURCE` pointing at Git repositories containing the selected locked commits; use fresh build directories. Alternate variants and rotate concurrency order as described above. For worker measurements use `--inline-version 0 --samples 30000 --warmup 128`; for summary use `--workloads summary --summary-samples 512 --warmup 8`. The migration command is `scripts/bench_migration.py --sizes-mib 1 8 32 64 --rounds 2`, repeated in alternating blocks.
