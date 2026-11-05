# Borrowed chunk-buffer measurements

The adopted change borrows incoming chunk bytes directly from `io_result::payload`
for one synchronous `transfer_receiver::append()` call. It removes the temporary
decoded string and result-value vector on the successful chunk path. The general
RPC decoder continues returning owning values. Wire bytes, validation, status
mapping, hashing, the four-chunk window and authority state machines are unchanged.

`migration::chunk_reply_view` contains a pointer, length and offset. The frame
must remain alive and unchanged while the view is used; temporary-frame overloads
are deleted. Parsing validates both envelope lengths, tags, counts and limits
before exposing the pointer. Error replies retain the ordinary checked-decoder
path. The receiver copies bytes into its own assembly storage before any view
expires. This is allocation-free, copy-free **chunk decoding**, not end-to-end
zero-copy networking. Bounds-checked little-endian scalar reads still use the
existing Dynabridge reader; no unaligned typed pointer dereferences are introduced.

## Final implementation

Measured on 2026-11-05 on the Ryzen 7 3700X, loopback TCP, two Hosts and a demo
coordinator. Both builds use GCC 16.2.1, Release `-O3 -DNDEBUG`, exceptions,
FF fast runner, erased invocations, libuv and OpenSSL SHA-256. LTO, native tuning
and sanitizers are disabled. The baseline is the preceding optimized owning
reply codec, not the original 190 ms implementation.

Five alternating pairs per size follow one discarded warm-up per build/size.
Size order rotates; fresh processes are used for every run. All 48 runs pass the
data, clone, authority/history and retirement checks. Compilation and sanitizer
checks finish before measurement. The demo uses its default polling pump and
phase instrumentation is off.

| Native MiB | Before Cut ms | After Cut ms | Before range ms | After range ms | After package MiB/s |
|---:|---:|---:|---:|---:|---:|
| 1 | 3.93 | 3.99 | 3.90–4.15 | 3.96–4.14 | 250.50 |
| 8 | 17.69 | 17.56 | 17.54–18.21 | 17.36–18.38 | 455.56 |
| 32 | 64.42 | 64.02 | 62.14–65.46 | 63.40–64.62 | 499.87 |
| 64 | 127.00 | 124.25 | 124.80–127.47 | 124.05–124.81 | 515.08 |

Values except ranges are medians. The 64 MiB median improves about 2.2%; smaller
sizes show little consistent change. An earlier isolated receive-only experiment
also showed a modest 64 MiB reduction, 125.18 to 123.81 ms. These are descriptive
small-sample results, not significance, p99 or saturation estimates.

A separate five-pair 64 MiB comparison uses a blocking demo pump and opt-in
CPU clocks; all ten measured runs and two warm-ups pass. Median chunk decode
thread CPU falls from 3.169 to 1.691 ms, and receive-loop CPU from 68.988 to
66.794 ms. Target hash remains about 35.6 ms. Instrumented Cut wall time is
136.43 to 133.92 ms. These timings include per-chunk clock overhead; sequential
clock reads can make CPU exceed wall time for very short spans. Nested spans
must not be added. No perf call-stack attribution is claimed.

## Sender experiment was not adopted

Writing source snapshot pointers directly into an owning send frame was also
implemented and tested. With both sender and receiver changes, source service
CPU fell from 45.18 to 33.83 ms, but end-to-end latency regressed. A ten-pair
confirmation measured 64 MiB Cut at 127.33 to 129.82 ms and 32 MiB at 65.12 to
66.90 ms. A separate sender-only comparison also regressed: 64 MiB was 127.47
to 131.00 ms. Removing local work did not shorten the whole asynchronous pipeline;
these measurements do not establish the exact scheduling/cache cause.

The final source encoder therefore retains the preceding owning path. The
unadopted implementation is retained as [sender-experiment.patch](sender-experiment.patch)
against the final source for reproducibility. It synchronously captures bytes
into an owning send frame before invoking completion; it never lends an export
pointer to asynchronous transport. It is an experiment, not a default option.

## Evidence and reproduction

- [comparison.json](comparison.json): final unprofiled size sweep, binary hashes,
  build metadata, raw samples and paired ratios.
- [profile.json](profile.json): final separate instrumented comparison.
- [receive-only-experiment.json](receive-only-experiment.json): initial isolated receiver run.
- [both-comparison.json](both-comparison.json), [both-profile.json](both-profile.json)
  and [both-confirmation.json](both-confirmation.json): rejected combined variant.
- [source-only.json](source-only.json): isolated sender experiment.
- [validation.json](validation.json): final local correctness checks.

```sh
python3 scripts/bench_migration_compare.py --before build-before --after build \
  --output build/buffer-comparison --rounds 5
python3 scripts/bench_migration_compare.py --before build-before --after build \
  --output build/buffer-profile --rounds 5 --sizes-mib 64 --profile --pump-mode blocking
```

The tests check pointer provenance, unchanged output on malformed input, frame
truncation/mutation, reassembly after frame destruction, duplicate/conflicting
chunks, Abort and late delivery. Allocation injection checks that valid and
malformed borrowed decoding work without heap allocation. Full Release tests
pass 51/51; the strict no-exception ASan/UBSan selection passes 7/7. No protocol
transition changed, so no new TLA+ model or rerun is required for this change.
