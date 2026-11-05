# Latest clipboard/migration comparison

Measured on 2026-11-05 on an AMD Ryzen 7 3700X (8 cores / 16 threads,
32 GiB RAM), using loopback TCP, two independent Hosts and a demo coordinator.
This comparison isolates the follow-up copy removal from the preceding
four-chunk pipeline/OpenSSL implementation. Both builds use GCC 16.2.1,
Release (`-O3 -DNDEBUG`), exceptions enabled, FF fast runner, erased invocations,
libuv and OpenSSL SHA-256; LTO/native tuning and sanitizers are disabled.

There are five measured pairs per size after one discarded run per build/size.
Build order alternates each round and size order rotates. All 48 runs, including
warm-ups, passed the data, authority, history and retirement checks. Each run
starts fresh processes; performance recording was disabled during these runs.

## Latency and memory

Times and RSS below are medians. The final implementation keeps four chunks in flight.

| Native MiB | Before Cut ms | After Cut ms | After Cut MiB/s | Source RSS before MiB | Source RSS after MiB |
|---:|---:|---:|---:|---:|---:|
| 1 | 4.59 | 4.16 | 240.43 | 16.65 | 14.05 |
| 8 | 20.05 | 18.68 | 428.28 | 36.97 | 28.52 |
| 32 | 99.95 | 96.80 | 330.59 | 108.38 | 76.52 |
| 64 | 188.48 | 189.62 | 337.52 | 203.79 | 140.05 |

The new implementation borrows the decoded chunk string for synchronous append
instead of copying it, and encodes the Dataset directly into its pre-reserved,
unpublished CPMG package. Early read-only capture uses the same package prefix;
final history is still appended only after Drain. Wire bytes, SHA-256, ownership,
Abort lifetime and Commit/Fence requirements are unchanged.

The 64 MiB source saves about 64 MiB of peak RSS. Its migration latency is
essentially unchanged in these paired samples: before 188.09–198.18 ms versus
after 189.04–191.78 ms, with a median paired before/after ratio of 0.995.
Do not compare the new median against a previous unpaired 199 ms sample and
claim a measured speedup. Median latency reductions at 1, 8 and 32 MiB are
approximately 9%, 7% and 3%, respectively. Five pairs are descriptive evidence,
not a stable tail-latency estimate or a statistical significance claim.

| Native MiB | Copy ms | Paste #1 ms | Paste #2 ms | Close → activation ACK ms | Target peak RSS MiB |
|---:|---:|---:|---:|---:|---:|
| 1 | 2.00 | 4.02 | 2.75 | 3.32 | 16.75 |
| 8 | 6.93 | 18.74 | 17.07 | 17.70 | 44.75 |
| 32 | 23.69 | 67.35 | 71.91 | 95.80 | 140.18 |
| 64 | 45.25 | 129.17 | 142.46 | 188.68 | 268.20 |

RSS is Linux VmHWM over the entire run, including two retained clones on the
target, not incremental migration memory. Cut includes reservations, Drain,
capture, transfer, validation/restoration, activation and cleanup. The observed
Close interval is coordinator Freeze submission to activation ACK, not exact
Host gate downtime. Query/validation RPCs are outside phase timers; the driver's
100 microsecond polling sleep is included. This idle-before-Cut workload does not
measure the benefit of overlapping capture with ongoing reads.

## Window experiment and profiling limit

A separate five-pair comparison tried eight outstanding chunks. Median Cut
latency was 18.84 → 18.89 ms at 8 MiB and 188.68 → 188.91 ms at 64 MiB.
There was no stable benefit on this loopback workload, so the shipped window
remains four. This does not establish the best window for a higher-RTT link.
No sixteen-chunk variant was promoted: the existing peer server queue is bounded
to eight frames, so that experiment would also require revisiting the queue contract.

Repository-signed perf packages were extracted under `/tmp` without a system
installation. A basic task-clock counter worked, but `perf record` requests for
user-space cycles and cpu-clock sampling were denied by the host. No CPU hotspot
percentages or call-stack attribution are claimed; no security settings were changed.

## Evidence and reproduction

[comparison.json](comparison.json) contains both builds' binary hashes, metadata,
all 40 measured samples, execution order and paired ratios. [report.json](report.json)
and [before.json](before.json) are the individual build summaries.
[window8-experiment.json](window8-experiment.json) records the separate window
trial. [demo.log](demo.log) is the final measured 64 MiB candidate run.
[validation.json](validation.json) summarizes the regression checks.

For one build, use the existing size sweep:

```sh
python3 scripts/bench_migration.py --build-dir build --output build/clipboard-bench --rounds 5
```

For alternating comparisons, retain the before/after executables and matching
CMake caches in separate directories, then run:

```sh
python3 scripts/bench_migration_compare.py --before build-before --after build-after \
  --output build-after/clipboard-comparison --rounds 5
```

These are sequential loopback operations, not physical-link throughput,
concurrent saturation or p99 measurements. See [the protocol documentation](../../../docs/object-migration.md).
