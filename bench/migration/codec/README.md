# Reply codec comparison

Measured on 2026-11-05 on an AMD Ryzen 7 3700X (8 cores / 16 threads,
32 GiB RAM), using loopback TCP, two independent Hosts and a demo coordinator.
Both builds use GCC 16.2.1, Release `-O3 -DNDEBUG`, exceptions, the FF fast
runner, erased invocations, libuv and OpenSSL SHA-256. LTO, native tuning and
sanitizers are disabled. The four-chunk transfer window is unchanged.

## Change and compatibility

Capstan replies still contain an ordinary Dynabridge RPC response whose string
payload is a request with method ID 900. Encoding writes both envelopes and
their values into one reserved buffer using Dynabridge's existing writer and
value encoder. Decoding validates the outer envelope and its exact length, then
uses the existing reader and value decoder directly on the inner frame. It no
longer materializes intermediate envelope strings/byte vectors or copies the
decoded argument tail. Each returned string retains its own storage; no borrowed
result crosses an asynchronous boundary.

Wire bytes, limits, status/outcome categories, malformed-input rejection, Drain,
hashing, transfer ordering and authority transitions remain unchanged. This is a
Capstan framing optimization; dependency revisions are unchanged. Failed checked
decoding leaves the caller's output unchanged. Tests compare bytes and acceptance
against the original composition of the pinned Dynabridge codecs, including
mutated/truncated frames, length/count limits and output lifetime after input
destruction. The new compatibility test also runs in sanitizer CI selections.

## Unprofiled end-to-end results

One discarded warm-up per build/size precedes five measured pairs. Build order
alternates each round and size order rotates. Fresh processes are used for every
run; all 48 runs passed data, clone, authority, retained-history and retirement
checks. No compilation ran during measurement. The demo uses its default polling
pump in both builds, and phase instrumentation is disabled.

| Native MiB | Before Cut ms | After Cut ms | After package MiB/s | Before range ms | After range ms |
|---:|---:|---:|---:|---:|---:|
| 1 | 4.06 | 4.06 | 246.63 | 4.04–4.19 | 3.94–4.12 |
| 8 | 18.28 | 18.16 | 440.53 | 18.11–19.05 | 17.46–18.43 |
| 32 | 95.32 | 66.60 | 480.48 | 93.63–100.12 | 65.34–67.86 |
| 64 | 190.09 | 127.24 | 502.98 | 187.14–190.94 | 125.52–128.51 |

All table values except ranges are medians. At 64 MiB, median Cut latency falls
33.1% and package throughput rises 49.4%; at 32 MiB, latency falls 30.1%.
Small-object Cut latency is essentially unchanged. These results cover both
encoding and decoding changes together, not an isolated speedup for either one.

The 64 MiB first/second Paste medians also fall from 130.85/141.76 ms to
87.41/74.71 ms. Copy remains about 44.6–44.8 ms. Peak RSS is essentially
unchanged: approximately 140 MiB source and 268 MiB target. RSS includes the
entire run with two retained clones; it does not measure allocation traffic.

## Separate instrumented comparison

Five additional alternating 64 MiB pairs, with one discarded warm-up per build,
use the blocking demo pump and opt-in phase CPU clocks. All 12 runs passed.
These samples include per-chunk instrumentation overhead and are not combined
with the unprofiled table above.

| Stage | Before thread CPU ms | After thread CPU ms |
|---|---:|---:|
| Source snapshot encoding and hash | 43.68 | 43.80 |
| Source chunk service through Fence | 109.29 | 45.04 |
| Target receive and hash | 133.31 | 70.83 |
| Target reply decode, nested above | 49.55 | 3.15 |
| Target append and hash, nested above | 41.88 | 42.04 |
| Target hash, nested in append | 35.63 | 35.60 |
| Target restoration | 10.35 | 10.33 |

The measured decoding span uses about 94% less CPU. Source chunk service includes
reply construction and transport, so its reduction cannot be assigned entirely
to the encoder. The remaining hash work is stable. Instrumented Cut wall time
falls from 202.19 to 139.44 ms; target receive wall time falls from 146.33 to
83.55 ms. Source service overlaps target receive, and nested spans must not be
summed. Sequential clock reads can produce CPU slightly above wall time for
very short per-chunk spans. This is CPU-clock instrumentation, not perf sampling.

## Evidence and reproduction

[comparison.json](comparison.json) contains build metadata, executable hashes,
all 40 unprofiled measured samples and paired ratios. [profile.json](profile.json)
contains the separate ten instrumented samples and raw phase clocks.
[validation.json](validation.json) records the local regression checks. The
[preceding Drain investigation](../profile/README.md) explains the target chosen
for this change.

Retain old executables and their CMake cache before rebuilding, then run:

```sh
python3 scripts/bench_migration_compare.py --before build-before --after build \
  --output build/codec-comparison --rounds 5
python3 scripts/bench_migration_compare.py --before build-before --after build \
  --output build/codec-profile --rounds 5 --sizes-mib 64 --profile --pump-mode blocking
```

Cut includes reservations, Drain, capture, transfer, validation/restoration,
activation and cleanup. These sequential loopback measurements do not estimate
p99, physical-link throughput or concurrent saturation. No protocol state machine
changed, so this codec-only change does not add or rerun TLA+ models.
