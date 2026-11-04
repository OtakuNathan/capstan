# Capstan

A C++14 experiment combining [lease](https://github.com/OtakuNathan/lease),
[Dynabridge](https://github.com/OtakuNathan/dynabridge), and
[Flux Foundry](https://github.com/OtakuNathan/flux_foundry) into a distributed
capability protocol. Resources stay on their authoritative Host. Clients send
capabilities, operation arguments and results; they do not migrate business objects.

**Experimental, not production-ready.** One Host per object, one directory control
session, trusted-lab networking, no HA or automatic recovery. The TCP demo binds
to loopback. There is no TLS or public-network security claim.

| Component | Responsibility |
|---|---|
| lease | Local exclusive/shared access and compile-time access constraints |
| Dynabridge | Static call contracts, typed object identity, encoding and dispatch |
| Flux Foundry | Local flows, external asynchronous awaitables and executors |
| Capstan | Grant protocol, directory, Host admission and operation lifetime |

A remote Host is an RPC destination, not an FF executor. RPC is request/completion
I/O; local FF executors schedule local work.

## Build and run

Requirements: Linux for TCP, CMake 3.20+, a C++14 compiler, Python 3.12+, Git,
pkg-config, GLib/GIO development files, and libuv development files for the UV
worker. No dependency installer or system service is run by the build.

```sh
# Explicit network step: fetch only the commits in deps.lock.json.
python3 scripts/fetch_deps.py

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCAPSTAN_UV_WORKER=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
# Table-managed shared/exclusive authority and destruction.
python3 scripts/run_demo.py --build-dir build
# Holder-initiated A -> B -> A, with Table stopped after bootstrap.
python3 scripts/run_direct_demo.py --build-dir build
```

The build exports locked commits into its own `_deps` directory. Existing sibling
checkouts (`../lease`, `../bridge`, `../flux_foundry`) are preferred when present;
working-tree edits are never imported. Override them explicitly with
`CAPSTAN_LEASE_SOURCE`, `CAPSTAN_DYNABRIDGE_SOURCE`, and `CAPSTAN_FF_SOURCE`.
When a lock changes, use a fresh build directory.

Without GIO, the protocol model, state machine, typed integration and callback
tests still build offline. The demo needs the TCP targets. Its supervisor creates
private temporary credentials, starts Host/Table/client A/client B, verifies
results and cleans up its processes. A 64 MiB Dataset is created locally on Host.

## Protocol guarantees and boundaries

Authority changes follow **Close → Drain → Install → Ack → Grant**. Host admission
and transitions share a serialization point. Accepted, queued and executing work
all hold pins; each object has one serial execution lane, even for requests from
the same exclusive holder. Installation and destruction wait for actual completion.

Host validates its full local grant record and method permissions. Shared access
cannot bypass write restrictions through a low-level RPC. Epochs and transaction
identity reject stale authority and conflicting replay. The Host completes the
control request after installation; Table does not poll an ACK timer.

Timeout, cancellation, disconnection, or dropping a client handle does **not** mean
the remote operation stopped. Results distinguish NotSubmitted, Rejected,
Completed, and OutcomeUnknown. Side-effecting calls are not automatically retried.
Acquisition and close are explicit; destructors cannot confirm remote release.

## Build policies

| Option | Default | Effect |
|---|---|---|
| `CAPSTAN_UV_WORKER` | OFF | Dedicated libuv business loop; network I/O stays on GIO |
| `CAPSTAN_FAST_RUNNER` | OFF | FF fast runner with no runner cancellation |
| `CAPSTAN_NO_EXCEPTIONS` | OFF | Compile Capstan/header dependencies with `-fno-exceptions` |
| `CAPSTAN_NO_EXCEPTION_STRICT` | ON | Require selected extension noexcept declarations in no-exception builds |
| `CAPSTAN_LTO` | OFF | Compiler-checked interprocedural optimization |
| `CAPSTAN_NATIVE_ARCH` | OFF | Tune for the build CPU; binaries may not be portable |
| `CAPSTAN_ASAN` / `CAPSTAN_TSAN` | OFF | Separate sanitizer builds; ASan also enables UBSan |

Strict and relaxed no-exception builds preserve permissions and Drain. Recoverable
protocol errors use checked results. Legacy must-succeed contract failures abort
without exceptions, including Release; ordinary standard-allocator OOM is
process-fatal. See [build and validation](docs/build-and-validation.md).

## Validation and measurements

- [Architecture and ownership](docs/architecture.md)
- [Protocol and failure semantics](docs/protocol.md)
- [Build policies, tests and CI](docs/build-and-validation.md)
- [Bounded TLA+ model, mutations and nightly CI](docs/formal-verification.md)
- [Latest measurements and raw data](docs/benchmarks.md)

Ordinary CI covers GCC/Clang optimized exception/strict/relaxed/fast builds,
compile-negative checks, offline tests, real multiprocess TCP, and selected
ASan/UBSan/TSan lifecycle tests. TLA+ runs nightly at 02:23 UTC and on manual
request. Passing a model, a sanitizer, and a TCP test are different kinds of
evidence; none substitutes for the others.

Current measurements are single-machine loopback TCP on a Ryzen 7 3700X. There is
no cross-machine validation, saturation-throughput guarantee, automatic failover,
object migration, distributed GC, or multi-object transaction support.

MIT licensed. Dependencies retain their own licenses; exact revisions are in
[`deps.lock.json`](deps.lock.json).

Holder-initiated exclusive transfer is implemented on the same Host drain barrier.
See [protocol and SDK](docs/direct-transfer.md),
[latest direct-transfer measurements](bench/direct-transfer/latest/README.md), and
[the narrated direct-transfer demo](docs/direct-transfer-demo.md).
`scripts/run_direct_transfer.py` provides the separate transfer fault suite.
