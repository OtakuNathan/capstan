# Local durable owner

The optional SQLite demo adds local restart recovery for one Dataset and its
Table-mediated authority changes. The ordinary memory Host, Copy/Paste and
migration demos keep their existing behavior. Accepted reads commit a result
receipt. Dataset writes use a versioned base snapshot plus ordered deterministic add_range redo records; the state head,
delta and request/result receipt commit atomically. Existing memory benchmark
numbers do not describe this backend.

Two state machines meet at explicit storage barriers:

- The protocol machine owns admission, pins, Close, Drain, Install, immutable
  Table intents, acknowledgements and result delivery.
- The storage machine prepares a candidate transaction, commits it durably, or
  loses the uncommitted candidate on crash. A commit result can be unknown to
  its caller even when the transaction has reached disk.

A successful result proves a durable operation, rather than just successful
submission. A lost response remains **Unknown** until the same request is
resolved. Neither disconnect nor timeout rolls back a committed operation.
There is no atomic transaction spanning a network round trip.

## Build and exercise

Install SQLite3 development files in addition to the normal TCP requirements
(`libsqlite3-dev` on Debian/Ubuntu; SQLite is available through Homebrew on macOS).
SQLite support is opt-in and does not affect the default build or offline users.

```sh
python3 scripts/fetch_deps.py
cmake -S . -B build-durable -DCMAKE_BUILD_TYPE=Release \
  -DCAPSTAN_SQLITE=ON -DCAPSTAN_REQUIRE_GIO=ON -DCAPSTAN_UV_WORKER=ON
cmake --build build-durable --parallel 2
ctest --test-dir build-durable -R '^(durable_.*|noexception_policy_matrix)$' \
  --output-on-failure
```

The two persistent executables use the same trusted-lab config and RPC encoding
as the memory demo:

```sh
build-durable/capstan_durable_host --config lab.conf --port 7001 \
  --state-file owner.sqlite --bytes 1048576
build-durable/capstan_durable_table --config lab.conf --host-port 7001 --port 7002 \
  --state-file table.sqlite
```

Use the existing client with ports 7001/7002 and its role credential. Keep the
same object identity, control session, config and database files across restart.
`--bytes` initializes a new object only; a restored object's saved size wins.
The database contains capability secrets; protect it like the lab config.
The two actors may use separate files or the same file with disjoint namespaces;
each transaction is still local to its actor, never a cross-actor protocol commit.

The interactive client is a lab tool: its counters restart with the process.
A persistent application must allocate unique operation IDs across client
restarts and retain the original request for retries. Reusing an ID for a new
logical operation is not supported. Retrying a committed operation requires the
same authenticated holder, capability and canonical arguments. A changed payload
under a committed ID returns Conflict, including after restart.

## Storage and publication rules

`sqlite_store` requires a file database, WAL journal mode and
[`synchronous=FULL`](https://www.sqlite.org/pragma.html#pragma_synchronous).
On macOS it also enables `fullfsync`. Each batch uses `BEGIN IMMEDIATE` and one
`COMMIT`; no transaction waits for network I/O. SQLite's
[atomic commit assumptions](https://www.sqlite.org/atomiccommit.html) still apply:
filesystem, VFS and device flushes must honestly implement durable storage.
The model assumes that guarantee; it does not prove the operating system or SSD.

One advisory actor lock is held for the lifetime of each frontend. A second
Host or Table using that same canonical database path refuses startup. Use one
canonical path, without hard-link aliases or replacing the live database file.
This is a local exclusion mechanism, not distributed fencing. Do not start a
second frontend from a copied database or remove live lock files.

The Host stores these independent responsibilities:

| Record | Durable contents | Publication boundary |
|---|---|---|
| Identity | Object key and storage schema | Before initial authority |
| Data | Base snapshot plus ordered redo and current version head | New delta and head in the same transaction as the operation receipt |
| Receipt | Authenticated holder/request ID, canonical request, encoded result | Before result delivery; replay never re-executes the operation |
| Authority | Gate, epoch, current grants, pending transition, accepted transition history | Before any installed ACK or use of a new grant |

A write validates its range/version and prepares its result, canonical receipt,
redo record and new head before COMMIT. After durable commit, the single object
lane applies the prepared unsigned add without allocation or exceptions. A
failed/ambiguous commit closes the frontend and does not publish the candidate;
restart resolves the actual committed head. The admitted Host pin remains held
until publication and posted native completion finish. This optimization is
specific to the deterministic Dataset add_range operation, not a generic
transactional wrapper for arbitrary C++ side effects.

The representation invariant is: applying each contiguous versioned redo to
`host/base` yields exactly the committed Dataset image named by `host/data`.
Recovery validates the version head, ordered deltas, ranges and each delta's
canonical successful receipt. Empty ranges still advance the version; word
arithmetic remains modulo 2^64. The abstract durable model's state+receipt
commit remains the specification; this concrete refinement is regression-tested,
not newly proven by the existing TLA model.

Legacy full-snapshot databases are upgraded atomically on first open. The new
`host/data` envelope is intentionally incompatible with the old snapshot decoder:
old binaries refuse startup instead of serving stale data. Keep a database backup
before upgrading; downgrade requires an explicit export/migration (not provided).
The base snapshot, redo records and their required request receipts are all
retained. Recovery requires each redo's receipt; neither may be pruned
independently. No log or receipt garbage collection is implemented. The existing
receipt limit bounds new accepted operations; restart also bounds redo loading
at 1,048,576 entries. Recovery time grows with the cumulative affected ranges.
Do not prune redo merely because a receipt is no longer wanted; checkpointing
and coordinated reclamation require a separate atomic protocol.

Close may overlap already admitted work. Its checkpoint records the pending
immutable transition and closed gate, without serializing volatile pins. The
final native completion retires its pin before Install. The runtime checkpoints
the resulting authority before flushing ACK waiters or starting work under the
new grant. With no active pins, Close and Install can collapse into one durable
checkpoint. No success acknowledgement is observable in the intermediate RAM
state. Checkpoint failure closes the frontend before that state can be used.

A restarted process has no surviving native operations. It restores committed
Dataset data and authority first, finishes a persisted Close with an empty Drain,
and durably checkpoints the installation before opening its server. Requests
without receipts have no committed effect and are not silently rerun at startup.
A later retry can execute only if its capability still passes current admission.
A committed request can replay its exact result under an old epoch; replay does
not grant new authority or start native execution.

The Table atomically stores its directory image, next transaction number,
immutable target, originating command and command-result history. It commits the
intent **before** sending SetAuthority. After a matching installed ACK and the
current-state check, it commits confirmation **before** returning the command
result. It continues to ask Host for current grants, rather than caching an old
ACK as current authority.

A recovered pending intent retains exactly the same transaction ID and grant
secrets. It is marked uncertain even if no send was recorded: the crash may have
occurred just after transmission. The existing `retry_admin` / `reconcile`
client commands (wire command 2003) reconcile that exact decision. A failed retry
cannot prove the original attempt was not submitted. No new target is minted,
no epoch is rebased, and no timeout releases the decision. Startup recovery does
not promise autonomous network retry or liveness.

Storage errors are sticky. An ambiguous commit never becomes a definite business
rejection. The frontend stops accepting work and pending observers receive
Unknown; executing work retains its real pin until backend completion. Reopen
and reconcile from durable records before resuming service. There is no automatic
retry of an ambiguous transaction against a newly allocated request ID.

## Model and implementation correspondence

[`DurableStore.tla`](../models/tla/DurableStore.tla) models committed disk,
volatile candidate transactions and process/power failures. It is composed by
[`DurableRPC.tla`](../models/tla/DurableRPC.tla), not mixed into the existing
migration or data-transfer machines. A model crash can stop Host, Table, or both
between any enabled actions. Network observations survive process death; volatile
pins and transaction buffers do not.

| Model action/state | C++ implementation |
|---|---|
| `Stage` / `Sync` / `Crash` | Prepared owning buffers; SQLite batch transaction; abrupt process exit and reopen |
| `Admit` / `host.queue` | `host_authority::admit`, including queued and executing operation pins |
| `PrepareWrite` | Backend validates a prepared add and allocates its redo, version head and request/result receipt |
| `Flush(Host)` for a write | One `sqlite_store::commit` batch atomically publishing redo, version head and receipt |
| `ObserveWriteCommit` | Backend finishes publication/resource access; completion can still be posted |
| `CompleteWrite` | Owner-loop `authority.complete`, followed by delivery outside authority locking |
| `Replay` | Durable receipt lookup before fresh admission; canonical-payload equality check |
| `PrepareIntent` / `Flush(Table)` | `table_runtime::advance` persists directory state and exact active operation before `start_io` |
| `SendConfigure` | SetAuthority RPC using the persisted target without reminting |
| `ReceiveConfigure` / `ObserveCloseCommit` | Shared authority Close; runtime checkpoint before returning to the event loop |
| `PrepareInstall` / `ObserveInstallCommit` | Final pin retirement, authority checkpoint, then `authority_replies::flush` |
| `PrepareTableConfirmation` / `PublishGrant` | Matching ACK/current-state observation, Table journal commit, then command completion |
| `RecoverHost` | Validate the base/head, replay contiguous receipt-bound redo, recover authority, then checkpoint any drained installation before serving |
| `RecoverTable` | Journal decode, restored sequence, exact pending target marked uncertain |

The projection fixes one logical object, one immutable Table authority change
(epoch 1 to 2), one authenticated business holder and one write lane. Its scalar
value represents increments to one Dataset word relative to the initial image;
write bodies are bounded to 1 or 2, with no integer wraparound in the model.
The stored result is the serial write version. It explores conflicting payloads
under the same request ID, rather than assuming callers always retry correctly.
Other methods, arbitrary ranges, grant sets, destroy semantics, serialization,
capacity limits and repeated authority epochs have separate C++ tests or existing
models; this bounded model does not prove all of those behaviors.

A disk image projects the database's separate records together. Updating only an
authority record preserves data and receipts; a write batch preserves authority.
SQLite serializes commits while preparation may overlap. Independent preparation
and immediate Close/Install are compressed steps at this abstraction level;
there is no claim of a machine-checked refinement proof for the C++ program.
The mapping and actual commit-boundary tests are maintained together.

Normal bounds are one request and one crash; nightly uses two requests and two
crashes. The normal safe model explores 1,344 distinct states and the nightly
model 48,439 (TLC v1.7.4, 2026-11-10). Eight mutants must violate the expected
invariant: early business ACK, missing atomic receipt, volatile-only dedup,
unbound retry payload, lost Table intent, missing Drain, incorrectly reopened
recovery and early installed ACK. Two additional witnesses establish reachable
lost-response recovery and recovered grant publication. These are bounded safety
and reachability checks, with no unbounded liveness or fairness theorem.

```sh
python3 scripts/check_tla.py --download-tools --suite durable
python3 scripts/check_tla.py --download-tools --suite durable --nightly
```

## Implementation evidence and limits

`durable_store_test` terminates child processes before/after COMMIT and verifies
that data and receipts recover together. It also checks local actor exclusion.
`durable_host_test` covers exact replay, conflicting payloads, authentication,
a persisted Close after worker commit but before pin retirement, installation
commit boundaries, and ambiguous-commit fail-closed behavior in exception builds.
`durable_table_test` uses actual loopback RPC and terminates both processes before
and after the intent/confirmation barriers, then restores the exact pending target
or committed result. Its failed retry keeps prior uncertainty intact.
`durable_lifecycle_test` covers 32 drained reopen cycles, eight observer-detach
and Close/Drain cycles, eight loopback disconnect/retry cycles, and eight crashes
after native publication but before owner-loop completion. It checks retained
backend lifetimes, exactly-once observer completion, and Linux fd/thread counts.

These tests use abrupt process termination, not a physical power cut. SQLite's
flush guarantee plus the model's power-loss abstraction support the intended
power-loss contract; actual hardware power interruption has not been tested.
GCC exception builds passed the complete 57-test suite; GCC strict/no-exception,
Clang relaxed/no-exception and ASan/UBSan checks cover the new paths. CI runs GCC
and Clang exception/strict/relaxed variants, while nightly also verifies the larger
formal bound. Runtime hooks have their own strict/relaxed compile-negative checks.

The first persistent frontend intentionally rejects holder-direct transfers,
Copy/Paste, migration, dynamic object routing and clone creation before they can
mutate state. Their crash recovery needs dedicated protocol composition and
journaling; a durable Dataset alone would not make those protocols durable.
The memory frontends continue to demonstrate those features.

There is no replication, automatic failover, split-brain protection, client-side
request journal, durable receipt eviction, general backup/restore or schema
migration framework, or repair of corrupted storage. The one-time legacy Dataset
format upgrade above and the physical SQLite layout upgrade below are the only
supported storage migrations. Do not discard
either actor's journal and reuse its old control session. Synchronous checkpoints
retain their durability contract; performance needs separate measurements.

## SQLite physical layout and statement lifetime

New databases use a rowid table with a narrow primary-key index, rather than
placing large Dataset base images in a WITHOUT ROWID key tree. Existing stores
with the exact legacy table definition and no additional explicit indexes or
triggers are copied and renamed in one BEGIN IMMEDIATE transaction. Unknown
legacy layouts are rejected rather than silently discarding schema information.
A crash exposes either the old or new complete mapping; keys and values are
preserved byte-for-byte. This first-open conversion requires writer exclusion,
extra disk/WAL space and time proportional to the retained database. Back up
before upgrading. This physical change does not remove the application-envelope
downgrade restriction documented above.

Get/put prepared statements are cached per store, used under its mutex, reset
and cleared on every exit, and finalized before database close. SQLite
[`prepare_v2`](https://www.sqlite.org/c3ref/prepare.html) handles schema
invalidation across cooperating connections. WAL FULL
and the business-state/receipt transaction boundary are unchanged. Tests cover
two-connection prepared statements across conversion, before/after-commit crash
cuts, large/empty values, and refusal of unknown legacy schema.

## Shared Dataset mutation semantics

The ordinary `add_range`, durable prepare/publication and snapshot redo now share
one range/version validator and one unsigned mutation loop. The direct call
preserves range-error precedence over version exhaustion. Publication checks its
prepared version/range and aborts on a broken exclusive-lane contract; after a
successful durable commit it remains allocation-free and noexcept. Recovery
instead rejects malformed input without changing the staged snapshot. Backend
format, receipt coupling and redo ordering checks remain separate. No on-disk
format or transaction-boundary change is introduced by this refactor.
