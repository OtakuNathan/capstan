# GCC / Clang toolchain comparison on Ryzen 7 3700X

No consistent latency advantage emerged for Clang in this experiment. Pooled medians are close, and the direction of the difference changes across the three ABBA cycles. This does not establish equivalence or a universal compiler ranking.

## Configuration and method

- Source commit: `bf9c80e60d616b0a15717f68e7a72850dbdc02ae` (exact Git archive, SHA-256 in the report).
- AMD Ryzen 7 3700X; loopback TCP, four real processes, 64 MiB dataset, seed 7, zero artificial work delay.
- GCC 16.2.1 20260810 with GNU ld 2.47; Clang 23.1.4 with LLD 23.1.4.
- Both: Release `-O3 -DNDEBUG -march=native -fno-exceptions`, strict no-exception checks, fast runner, libuv worker, CMake IPO enabled.
- CMake selects GCC `-flto=auto -fno-fat-lto-objects` and Clang `-flto=thin`. This compares toolchains including their linker and LTO implementations, rather than isolating compiler code generation alone.
- Three ABBA cycles: `[GCC, Clang, Clang, GCC] * 3`, each block using a fresh cluster. Both builds and their validation finished before measurements began. Business workload and transfer-path order alternate across blocks.
- Per compiler: 6,000 measured version calls, 600 summary calls, and 360 handoffs per path, after warmup. No failed runs were discarded.
- Each toolchain passed all three selected runtime tests: `four_process_demo`, `direct_transfer_demo`, `tcp_direct_transfer`.
- The benchmark verifies identical summaries, epochs, recipient usability, zero Table traffic during direct handoffs and warm business calls, and final drain/accounting invariants.

## Pooled latency

All values below are microseconds. Lower is better.

| Metric | GCC p50 | Clang p50 | GCC p95 | Clang p95 | GCC p99 | Clang p99 |
|---|---:|---:|---:|---:|---:|---:|
| business/version | 44.926 | 44.624 | 71.573 | 70.883 | 119.237 | 126.995 |
| business/summary | 2741.940 | 2695.334 | 5249.618 | 4978.882 | 6582.920 | 7008.585 |
| direct/installed | 44.414 | 44.634 | 71.061 | 65.652 | 110.366 | 81.590 |
| direct/ready_components | 81.889 | 81.651 | 126.045 | 113.451 | 185.723 | 140.324 |
| direct/ready_wall | 171.519 | 166.696 | 269.415 | 236.053 | 931.654 | 551.465 |
| table/ready_components | 198.052 | 201.813 | 317.138 | 295.780 | 819.106 | 852.233 |
| table/ready_wall | 277.025 | 282.632 | 453.534 | 422.366 | 1105.967 | 1087.042 |

`installed` measures the transfer request through its response. `ready_components` sums each handoff's transfer and recipient claim/fetch client timings; it includes recipient materialization but excludes the Python driver's gap between commands. `ready_wall` measures through recipient readiness using the driver's clock, including that gap. These are quiescent transfers, not transfers competing with an in-flight scan.

## Variation across cycles

Clang/GCC ratios of block medians within each ABBA cycle (below 1 favors Clang):

| Metric | Cycle 1 | Cycle 2 | Cycle 3 |
|---|---:|---:|---:|
| business/version | 1.0156 | 0.9871 | 0.9741 |
| business/summary | 1.0089 | 0.9111 | 1.0017 |
| direct/ready_components | 0.8565 | 0.9291 | 1.1558 |
| table/ready_components | 0.9895 | 0.9273 | 1.0869 |

Summary's pooled p50 improves by 1.70%, but its cycle comparisons change direction. Direct readiness component p50 differs by only 0.29%, while cycle comparisons vary substantially. Tail latency also gives mixed results; these three cycles are descriptive evidence, not a statistical significance claim.

The host executable's actual `.text` section is 126,161 bytes with GCC and 102,521 bytes with Clang: 18.74% smaller with Clang. `.rodata` is 3,808 versus 3,411 bytes. These are section sizes, not total executable sizes, measured using `size -A`. The report also preserves GNU `size` aggregate columns, whose `text` value is broader than the `.text` section.

This is one machine with normal scheduling and existing background activity. CPU affinity and power policy were unchanged; block-level load/frequency snapshots are preserved. Logging and Python pacing remain enabled. Do not compare these absolute latencies directly with older published measurements as if they were matched runs, or attribute differences to vectorization without separate evidence.

## Evidence and reproduction

- [Report](report.json): quantiles, all block results, versions, flags, hashes, and semantics.
- [Raw samples](raw.csv): all 20,400 metric rows (multiple metrics per handoff).
- [Build and validation commands](builds.json); [GCC test log](gcc-test.log) and [Clang test log](clang-test.log).
- [Build script](build.py) and [benchmark script](compare.py).
- Generated host compile/link commands: [GCC flags](gcc-flags.make), [GCC link](gcc-link.txt), [Clang flags](clang-flags.make), [Clang link](clang-link.txt).

Machine-specific experiment and dependency paths in published metadata/logs are
replaced with `<experiment-root>` and `<dependency-root>`. Measurements, hashes,
and compiler options are unchanged. CSV line endings are normalized to LF, and
trailing blank lines in generated flags files are removed. The benchmark script is byte-identical to
the measured script; the build helper only adapts root/dependency paths for
reproduction. The full local archive retains the original logs and metadata.

From a Capstan checkout containing this evidence, use a fresh experiment directory:

```sh
experiment_dir=$(mktemp -d)
git archive --format=tar.gz bf9c80e60d616b0a15717f68e7a72850dbdc02ae > "$experiment_dir/source.tar.gz"
tar -xzf "$experiment_dir/source.tar.gz" -C "$experiment_dir"
cp bench/compiler-comparison/latest/build.py bench/compiler-comparison/latest/compare.py "$experiment_dir/"
cd "$experiment_dir"
python3 scripts/fetch_deps.py
python3 build.py
python3 compare.py
```

This requires both compiler toolchains, CMake, Python, and Capstan's native
build dependencies. Run on the same CPU with the recorded toolchain versions
for the closest comparison; `-march=native` targets the machine running the
build. Output goes to `results/`. Rerunning overwrites that directory's experiment
results, so retain evidence first. Source archive hashes also depend on Git's
archive/compression output; the source commit identifies the measured revision.

Local verification recomputed all reported p50/p95/p99 values and sample counts
from the CSV, matched the original source archive and benchmark script hashes,
and checked every build/validation exit status. Publication checks also verified
that path redaction left all measurements intact.
