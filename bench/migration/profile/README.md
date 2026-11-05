# Drain and data-path CPU measurements

Measured on 2026-11-05 on the Ryzen 7 3700X over loopback, with a 64 MiB
Dataset, two independent Hosts and a demo coordinator. The Release build uses
GCC 16.2.1, `-O3 -DNDEBUG`, exceptions, the FF fast runner, erased invocations,
libuv and OpenSSL SHA-256. LTO, native tuning and sanitizers are off.

Each of four occupancy cases runs with both demo pump modes. One run per case
and mode is discarded, followed by five measured rounds with rotating order:
40 measured runs and eight warm-ups. Every run passed data, clone, retained
history and retirement checks. The script verifies actual admitted occupancy
at Freeze and checks executable hashes before and after the sweep.

## Drain is waiting for resource users

The following medians use the blocking demo pump. Drain is measured from entry
to the Host Freeze handler until the Host observes zero business pins. The full
barrier additionally requires zero capture pins. These are local observations,
not exact mutex linearization timestamps.

| Occupancy at Freeze | Business Drain wall ms | Full barrier wall ms | Drain owner-thread CPU ms | Drain process CPU ms |
|---|---:|---:|---:|---:|
| Idle | 0.0115 | 0.0150 | 0.0122 | 0.0118 |
| One read, 100 ms injected hold | 103.0703 | 103.0794 | 0.0878 | 12.9456 |
| One write, 100 ms injected hold | 99.7943 | 99.8002 | 0.0671 | 0.2053 |
| Four queued reads, 25 ms hold each | 112.6739 | 112.6832 | 0.2052 | 22.3845 |

Injected holds use worker sleep, followed by the real business method; they are
not 100 ms of computation. Reads execute Dataset summary, while writes execute
a zero-delta update that still advances the version. The existing business lane
serializes queued work. Read process CPU includes useful summary work and early
snapshot capture: prefetch takes 9.64 ms for the single-read case and overlaps
Drain. Final source encoding/hash then takes 34.52 ms, versus 43.93 ms when idle.
No data/history publication or authority transition moves before the barrier.

The write case spends roughly 99.8 ms waiting while using only 0.067 ms of
owner-thread CPU. There is little busy-wait CPU to remove from this Drain path.
This does not prove a universal latency lower bound: changing business work,
queue scheduling or cancellation can change how long resources remain in use.
Tiny spans can report CPU slightly above wall time because clock reads occur
sequentially rather than atomically.

## The demo pump has avoidable wakeups

Only the coordinator's pump changes. Both Hosts continue using blocking
GMainLoop dispatch and event-driven completion.

| Case | Cut ms, poll / blocking | Driver CPU ms, poll / blocking | Empty pump turns, poll / blocking |
|---|---:|---:|---:|
| Idle | 204.48 / 200.03 | 3.166 / 0.302 | 1321 / 0 |
| One read | 295.67 / 295.91 | 4.764 / 0.360 | 1912 / 0 |
| One write | 302.86 / 300.41 | 4.834 / 0.327 | 1961 / 0 |
| Four queued reads | 305.37 / 304.01 | 5.638 / 0.390 | 1971 / 0 |

Blocking removes the demo's 100 microsecond polling sleep/wake cycle and saves
CPU. The five-run latency ranges overlap; this is not evidence of a stable
end-to-end speedup. Poll remains the default for compatibility with the existing
benchmark series; `--pump-mode blocking` selects the measured alternative.

## Most remaining time is active data processing

Idle/blocking medians:

| Stage | Wall ms | Measuring-thread CPU ms |
|---|---:|---:|
| Source encoding and hash | 43.93 | 43.76 |
| Source hash, nested above | 34.28 | 34.14 |
| Target receive and hash | 144.25 | 131.97 |
| Target reply decoding, nested above | 49.07 | 48.94 |
| Target append and hash, nested above | 41.97 | 41.83 |
| Target hash, nested in append | 35.69 | 35.58 |
| Target restoration | 10.36 | 10.29 |

The receive loop uses about 132 ms of CPU in 144 ms of elapsed time; its remaining
wall time is not automatically waste. Append excluding its nested hash accounts
for roughly 6 ms. Other receive-loop CPU includes transport, allocation,
request construction, callback dispatch and measurement overhead. Source chunk
service overlaps target reception, so their wall times must not be summed.

Code inspection of `wire::try_decode_reply` shows an outer response containing
an encoded inner request, owning string-to-byte conversions, and copying the
decoded argument tail into the reply. Removing these intermediate payload
copies is a concrete next candidate. The 49 ms measures the complete decoding
span, not copies alone; these measurements do not establish how much of it can
be eliminated. No decoder or wire-format change is included in this profiling
work.

The subsequent [reply codec comparison](../codec/README.md) implements this
candidate and records independent before/after results. The measurements on
this page describe the earlier implementation.

## Evidence and reproduction

[report.json](report.json) contains all samples, min/median/max summaries, build
metadata and executable hashes. [validation.json](validation.json) records local
checks. Instrumentation is opt-in and includes per-chunk clock overhead. These
results are separate from the [unprofiled comparison](../latest/README.md).
They are CPU-clock measurements, not perf samples: the machine denied sampling
permissions, so no call-stack or hardware-counter attribution is claimed.

```sh
python3 scripts/profile_migration.py --build-dir build \
  --output build/drain-profile --rounds 5

# One diagnostic run:
python3 scripts/run_migration_demo.py --build-dir build \
  --output build/write-drain --bytes 67108864 --profile \
  --inflight write --work-delay-ms 100 --pump-mode blocking
```

Five samples per case describe these runs; they do not estimate p99, physical
network throughput or concurrent saturation. This diagnostic work does not
change the authority or data-transfer state machines.
