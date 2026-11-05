# Concurrency and execution policy

Capstan currently serializes business execution for one Dataset. More TCP
connections can overlap transport and admission, but do not create more execution
lanes for that object. Concurrent connections are not CPU worker threads.

The [latest measured comparison](../bench/concurrency/latest/README.md) separates
socket timeout policy, descriptor watches, scalar placement and the empty ACK
queue fast return. It includes every round and raw sample, not only peak rates.

The Linux transport uses GLib descriptor sources for its owned, nonblocking
sockets. Connection establishment retains a five-second timeout. The inherited
data-socket timeout is explicitly cleared after connecting. There is no automatic
established-session timeout: callers can install a timer that triggers the SDK's
`detachable_endpoint::timeout_pending()`, which reports `Unknown` locally while
preserving the underlying operation's ownership. This matters because [GSocketClient's timeout](https://docs.gtk.org/gio/method.SocketClient.set_timeout.html)
also applies to sockets it creates, and [GSocketSource](https://docs.gtk.org/gio/method.Socket.create_source.html)
can turn expiry into a subsequent I/O failure. Descriptor sources avoid that
per-socket readiness/timeout bookkeeping. Socket
framing, connection identity, partial reads/writes, queue bounds, duplicate reply
handling and cross-thread completion delivery are unchanged. Each source is
destroyed before its socket is closed.

Worker-to-context callbacks publish their closure and preceding business writes
through an explicit C++ release/acquire edge. This makes the handoff visible to
TSan even when the system GLib library is not instrumented. Posting always queues
a source; it does not execute the callback on the submitting worker.

## Host options

| Option | Default | Behavior |
|---|---:|---|
| `--inline-version` | `1` | Execute the fixed, bounded scalar getter on the I/O thread after normal admission and lane selection. `0` retains worker dispatch. |
| `--trace-requests` | `1` | Emit ADMIT, COMPLETE and REJECT lines. Set to `0` to exclude per-request formatting and flushing from a throughput run. Authority/control diagnostics remain enabled. |
| `--history-limit` | `4096` | Maximum retained business request records and, separately, authority transaction records. Allowed range: 1–1,048,576. Records are allocated as needed; the limit is not a preallocation. |

Inline scalar execution retains the same authorization checks, deduplication,
admitted pin, FIFO execution lane and posted completion. It cannot overtake an
already executing operation. Scans and writes remain on the worker, and any
nonzero injected `--work-delay-ms` also keeps scalar reads on the worker. Only
the known nonblocking `version` method qualifies; this is not permission to run
arbitrary business callbacks on the network thread.

Completion still releases the Host pin before acknowledging a drained authority
transition. Direct and Table-mediated handoffs use the same Close → Drain →
Install sequence. Local execution placement does not bypass the barrier.
When no authority replies are waiting, the reply adapter returns before creating
its temporary 64-slot reply array. Nonempty queues retain the existing detach,
receipt selection and callback ordering.

## Retained history is a finite lifetime budget

Business request records are retained after completion and across authority
epochs. Reads also consume them. They share the configured budget across holders,
rather than receiving a fresh budget per connection. After the budget is full,
new business calls return `exhausted`; control queries remain available. The
64-operation active queue and 64-connection transport limits are separate bounds.

The default 4,096-record budget is appropriate for bounded demonstrations, not
indefinite service. Raising the budget permits a longer measurement window, at
the cost of retained memory. It does not implement history retirement, recovery,
or an indefinitely reusable session. Silently evicting records would weaken
duplicate-execution protection and is not an optimization implemented here.

## Reproduce a native concurrency sweep

Build with the locked dependencies and the same options for every comparison:

```sh
python3 scripts/fetch_deps.py
cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release \
  -DCAPSTAN_NO_EXCEPTIONS=ON -DCAPSTAN_NO_EXCEPTION_STRICT=ON \
  -DCAPSTAN_FAST_RUNNER=ON -DCAPSTAN_UV_WORKER=ON \
  -DCAPSTAN_LTO=ON -DCAPSTAN_NATIVE_ARCH=ON
cmake --build build-perf --parallel 2
python3 scripts/bench_concurrency.py --build-dir build-perf \
  --output work/concurrency-native --workloads version,summary
```

The native `capstan_throughput` driver uses authenticated TCP, typed Capstan
invocations, FF and Lease. Every connection has one outstanding request; a
completion schedules its replacement on the driver's GLib loop. Python handles
setup and supervision, not per-request pacing. Returned versions and summaries
are validated. Raw samples are written after each measurement window.

For longer finite batches, explicitly increase both the request count and history
budget. Warmup and two validation calls must also fit in the budget:

```sh
python3 scripts/bench_concurrency.py --build-dir build-perf \
  --output work/concurrency-long --samples 50000 --warmup 1000 \
  --history-limit 65536 --inline-version 1 --trace-requests 0
```

Use `--inline-version 0` for the worker-placement control. Output directories are
not overwritten unless `--overwrite` is supplied. Reports preserve configuration,
binary and relevant source hashes, batch times, successful RPC rates, p50/p95/p99,
result checks and final drained Host accounting. The default tracing policy in
the benchmark is explicitly off, while the Host executable's default remains on.

This is a closed-loop, finite-batch test on one shared object. It includes batch
ramp-up/drain and the native driver's scheduling overhead. It does not establish
open-loop tail latency, multiple-object scaling, cross-machine capacity or the
machine's absolute architectural limit. Avoid mixing latency from differently
paced drivers or counting fast rejections as successful throughput.

## Reading perf evidence

Profile warmed business windows separately from uninstrumented latency runs.
Record the Host and load-generator PIDs, and separate the Host's I/O thread from
its worker. On systems allowing only user-space sampling, useful commands are:

```sh
perf record -e cycles:u -F 999 --call-graph dwarf,8192 --user-callchains \
  -p "$HOST_PID,$LOAD_PID" -o perf.data
perf report --stdio --no-children -g none -i perf.data
perf report --stdio --children -g graph,1,caller -i perf.data
```

The experiments use perf's control-pipe enable/disable acknowledgements and a
driver gate so initialization, warmup and sample-file output are excluded. Inspect
per-thread `/proc` user/system CPU ticks and context-switch deltas as well: a
user-cycle profile cannot explain kernel CPU or off-CPU waiting by itself.
Children percentages overlap and cannot be added. Preserve unresolved symbols
and lost-sample counts rather than assigning unknown addresses to guessed costs.

Runtime checks include both scalar placement modes, silent request tracing,
write/read results across direct handoffs, and history exhaustion that remains
exhausted after another authority transfer. Transport fragmentation, coalescing,
duplicate callbacks, disconnects and backpressure retain their existing tests.
These checks and the native benchmark smoke run are part of CI and the sanitizer
selection; they supplement the existing bounded TLA+ authority models.
