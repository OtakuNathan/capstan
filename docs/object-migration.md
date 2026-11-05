# Copy, Cut and Paste

Capstan's experimental C++ clipboard API and three-process CLI demo transfer
Dataset values and relocate live object authority over the existing authenticated
TCP protocol. They do not depend on X11, an operating-system clipboard, or a new
GPU executor. Dynabridge supplies binary framing and typed invocations; FF runs
the asynchronous calls, and Lease still governs local access.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
python3 scripts/run_migration_demo.py --build-dir build --output build/clipboard-demo
```

The script starts two independent Host processes and a session-owned coordination
driver. It copies a value, modifies the source, pastes two independent objects,
then migrates the original object and verifies its data, retained request history
and retired source storage. Logs and a JSON measurement are written to the output
directory; temporary credentials are removed afterwards. `--bytes 8388608` selects
an 8 MiB Dataset. The measurement is one end-to-end sample, not a saturation
benchmark. Its observed Close-to-activation interval starts when the coordinator
sends Close and ends when it receives the activation ACK. Linux reports Host peak
RSS separately; it is not the same quantity as the configured buffer quota. The
[latest buffer measurements](../bench/migration/buffer/README.md) retain build settings,
per-phase timings and repeated samples. To reproduce the size sweep, use:

```sh
python3 scripts/bench_migration.py --build-dir build --output build/clipboard-bench --rounds 5
```

This runs 1, 8, 32 and 64 MiB objects, discards one warm-up run per size, then
rotates size order across five measured rounds. Each run starts fresh Hosts and
checks Copy-time data, independent clones, retained migration history and source
retirement. Results report medians and min/max, without inferring p99 from five
samples. Copy and each clone Paste have separate timers; validation/query calls
are outside those timers. The driver's 100 microsecond polling sleep remains
part of the observed operation latency.

To compare two already-built implementations with alternating execution order:

```sh
python3 scripts/bench_migration_compare.py --before build-before --after build-after \
  --output build-after/clipboard-comparison --rounds 5
```

Each build directory must contain both executables and its matching CMake cache.
The comparison records binary hashes, runs each pair in alternating order between
rounds, and reports paired ratios as well as per-build latency/RSS summaries.
Use `--profile --pump-mode blocking` for a separate instrumented comparison;
these samples include timing overhead and should not be pooled with unprofiled
latency results. The [earlier buffer-copy comparison](../bench/migration/latest/README.md)
retains the preceding optimization's latency and memory evidence.
The [owning reply codec comparison](../bench/migration/codec/README.md) records
the baseline for the latest borrowed chunk decoder.

For opt-in wall/thread-CPU/process-CPU measurements of Drain, capture, hashing,
reply decoding and restoration, use:

```sh
python3 scripts/profile_migration.py --build-dir build --output build/drain-profile --rounds 5
```

This compares idle, one admitted read, one admitted write and four queued reads,
with both polling and blocking demo pumps. Synthetic worker holds make actual
pin lifetimes observable; correctness and occupancy are checked on every run.
The [Drain profiling report](../bench/migration/profile/README.md) explains the
measurements, nested spans and instrumentation overhead. Host timing is disabled
by default. This is separate from the unprofiled latency comparison above.

## Public operations

`include/capstan/access/clipboard.hpp` provides owning, single-use invocations:

```cpp
// source and destination are shared, authenticated io_endpoint owners.
auto capture = capstan::copy(source, readable_capability);
// Run capture with FF to obtain migration::snapshot_ref.
auto clone = capstan::paste(destination, snapshot, fresh_object_key,
                           paste_sequence, source_address, source_port);
// Run clone with FF to obtain the clone's exclusive grant.
```

Both read and exclusive capabilities can Copy. A local read borrow is captured
at construction and ends before successful completion reaches the continuation.
The source Host admits a real read pin into the same FIFO lane as other Dataset
operations. The resulting immutable snapshot represents the value at that
capture point. Copy completion leaves the source open; later source writes cannot
change the saved value. A duplicate Copy request returns the same capture. The optional `copy_ref&`
output preserves a non-secret pending identity before submission; `query_copy`
resolves a lost Copy reply without recapturing newer data. `query_clone`,
`abort_clone` and `release_snapshot` provide typed reconciliation and cleanup.
A NotSeen observation alone does not prove that a previously submitted request
cannot arrive later.

The public snapshot reference contains source identity, Host instance, holder,
sequence, length and digest, **not a grant secret**. A reference alone is not
permission: snapshot access is bound to the authenticated holder (or trusted
controller). The clone's peer requests use the requesting holder's credential,
never the controller credential at a caller-selected address. One snapshot can be pasted repeatedly using distinct Paste sequences
and fresh object keys. Each clone receives fresh exclusive authority and its own
request history. Neither grants nor source request deduplication are copied into
a clone. Duplicate Paste returns the original creation receipt; it cannot
republish an object subsequently migrated or revoked. That historical receipt
is not a current-location or current-grant query.

A Cut uses `cut_plan::prepare(capability, move_operation, status)`. Preparation
is inert. `plan.paste()` seals the local exclusive facade only when FF submits
the invocation; destroying an unsubmitted invocation does not seal it. Invoke
`plan.reconcile()` after an uncertain result to retry the same operation, or
`plan.abort()` before Commit to request ordered rollback. The old facade never
reopens. A completed Abort issues a fresh source grant that must be materialized
as a new capability. A `cut_ref` is a non-authorizing public identity; the private
migration ticket carries the source grant and stays inside the trusted controller.

These APIs require the transport's owner context. Fresh invocations are Blueprint
inputs; never capture a consumed invocation in a reusable Blueprint. The normal
Copy/Paste path retains its callable type; the erased build explicitly projects
it to an erased invocation. The Cut factories also expose concrete invocations
that callers can explicitly erase. Construction and callback storage may allocate;
no-exception builds retain the existing strict/relaxed policies and process-level
standard-allocator OOM behavior.

## Migration ordering and reconciliation

`migration::move_operation` is a session-owned Table record. It is the coordinator
used by the demo driver, separate from the legacy `capstan_table` executable's
single-object directory commands. Keep its record and endpoint owners for the
entire session, including after Unknown. Ordinary endpoint operations and this
record run on their owning context; they are not arbitrary-thread entry points.

The coordinator performs:

1. Source/destination buffer reservations and data peer connection setup, before Close.
2. Source Close and actual Drain, including already admitted queued work; a read-only
   admitted set permits snapshot capture to overlap this Drain under its own pin.
3. Final authority/history capture after Drain, reusing the early Dataset snapshot
   when available; otherwise capture the Dataset under a separate backend pin.
4. Direct destination-to-source chunk requests; the controller sees metadata only.
5. SHA-256/schema validation and restoration into an unpublished Prepared object.
6. An immutable Table Commit decision, then source Fence and its ACK.
7. Destination activation and ACK, followed by source retirement and export release.

The same logical object key survives migration. Its placement generation and
permission epoch advance, and the full bounded authority/dedup history moves with
it. Moving A→B→A cannot erase old request or holder-transfer evidence. The source's
admission mutex arbitrates migration, directory transitions, holder transfers
and ordinary RPCs. Host pins survive client disconnects and observer timeouts.

Unknown retains the last checkpoint. A lost activation ACK retries activation;
it does not authorize rollback. Once Commit is recorded, Abort is rejected.
Before Commit, Abort first establishes a destination tombstone, then restores the
source with a fresh grant after its last real resource user finishes. A capture
pin therefore delays source restoration even after the ordinary business Drain.

Ready and cleanup are distinct: `move_operation::ready()` means activation was
acknowledged, even if source cleanup later needs reconciliation. An old retirement
command cannot delete a later placement. A repeated activation returns Superseded
when that placement/grant is no longer current; a historical installation ACK
must not be used to publish an obsolete route.

The Host registry routes explicit object keys and hosts multiple independent
objects. Clones stay unpublished while receiving. Migration reserves the target's
authority gate before receiving bytes, preventing a competing ordinary grant
from opening it early. Source and target instances are bound into control and
data identities. `--host-id` must differ between migration peers; instances are
OS-random per process unless `--host-instance` is explicitly supplied.

## Storage, ownership and limits

The default `--migration-buffer-bytes` is 512 MiB per Host (1 KiB–1 GiB accepted).
The registry aggregates native Dataset bytes, retained Copy snapshots, migration
exports, receive buffers and staging reservations across its objects. Both sides
reserve before Close. Source reservations conservatively include Dataset framing
and all bounded history entries; final packages must fit that reservation.
Metadata maps, transport frame buffers, allocator overhead and worker stacks are
separately bounded resources, not an exact resident-memory accounting guarantee.

There are at most 64 dynamic object slots, 64 clone transaction records, and 64
Copy/export/import records per object. Authority history remains bounded by the
configured history limit. Exhaustion rejects new work. Terminal histories and
unpublished aborted slots are retained rather than silently evicted. Explicit
release drops a snapshot/export buffer without reopening permissions. Releasing
a Copy snapshot during another Paste can cause that incomplete Paste to fail;
it cannot publish partial data. Abort that Paste explicitly to reclaim its
reservation. Its late response cannot resurrect it.

Backend state and observer delivery have separate lifetimes. The shared receiver
retains its callback token, and restoration completes before its payload storage
is released and before a downstream callback runs. A transport timeout does not
remove a native capture pin. Host runtimes and registries are process-lifetime
owners in this demo; shutdown while callbacks are outstanding is not a public
hot-destruction API.

Incoming chunk decoding borrows a pointer and length from the completion's
owning frame. `transfer_receiver::append()` copies those bytes synchronously
before the frame is released; the view is never retained by the receiver or
passed to another asynchronous operation. General RPC results remain owning.

## Binary formats and commands

Dataset snapshots use `CPSN`, little-endian u32 schema 1, followed by TLVs with a
u32 tag and u64 byte length. The high tag bit marks required fields. IDs 1, 2 and
3 encode Dataset type 1, business version and packed little-endian u64 words.
There are at most 64 fields, 256 MiB of words and 4096 extension/header bytes.
Duplicate known fields, unknown required fields, truncation and unsupported
versions reject without replacing the caller's output. Native pointers and
object-layout bytes never cross the wire.

Migration history uses `CPAU` with checked existing control encodings. A `CPMG`
package contains schema 1, a length-prefixed Dataset snapshot and a length-prefixed
authority image. Manifest schema 1810 binds the full package SHA-256, both Host
identities/instances, controller transaction, object and generation. Internal
ticket schema 1812 includes a grant secret. Public snapshot-reference schema 1820
does not. Blocks are 48 KiB except the tail, inside the existing 64 KiB transport
frame limit. Receivers reject changed duplicates and changed manifests; Seal
remains pending while any chunk is missing. Each data peer connection permits
four outstanding chunk requests. Ordinary RPC
clients retain a one-exchange default. Chunks may arrive out of order; a bitmap
tracks presence, identical duplicates are idempotent, and conflicting duplicates
are rejected. SHA-256 consumes only the contiguous received prefix, so the
original whole-package digest and wire format remain unchanged.

| Commands | Meaning |
|---|---|
| 4100–4104 | Source Freeze, query, Fence, Abort, Host discovery |
| 4105–4108 | Export, chunk fetch, explicit export release, source reservation |
| 4110–4115 | Target reservation, peer pull, query, Abort, activation, source retirement |
| 4200–4203 | Copy capture, snapshot chunk fetch, snapshot release, Copy query |
| 4210–4212 | Paste clone, creation query, clone Abort |
| 4900 | Explicit object-key route around an existing command |

Existing unwrapped single-object calls and method IDs remain compatible. The
registry reuses authentication and decoding instead of repeating envelope work on
the original RPC path. These are trusted-lab authenticated sessions, with the same
transport security assumptions as the rest of Capstan.

## Executable and formal evidence

Tests cover actual worker Drain, capture/Abort reentry, changed retries,
authentication, returning migrations, Copy-time data, clone isolation, aggregate
quota rejection, routed clone migration, caller-bound references, explicit
release, and an Abort racing a held chunk response. Lost reservation and activation
ACKs exercise same-session recovery. The CLI test uses independent processes.

`DataTransfer`, `ObjectMigration`, `ImportReservation`, `Clipboard`,
`PlacementReplay` and `LeaseCopyCut` supplement the existing authority/direct-transfer
TLA+ models. The runner requires ten safe models, thirty-eight expected mutant
counterexamples and five reachability witnesses (53 checks). Nightly also raises
the Lease reader, prior-pin and value bounds. CI runs the bounded checks. These
are bounded protocol abstractions, not proofs of C++ memory safety, authentication,
hash strength, general liveness or crash recovery.

### Independent data-transfer state machine

`WindowedDataTransfer.tla` models the new bounded sender window, arbitrary chunk
arrival, ordered hash progress, duplicate integrity, identity separation and
terminal Abort independently of authority. `DataTransfer.tla` retains its smaller
receiver abstraction. Neither model adds transport state to `ObjectMigration`,
`LeaseCopyCut` or `Clipboard`.

Six transport mutants disable the window bound, identity check, duplicate check,
complete-data requirement, abort tombstone or ordered hash prefix. A reachability
witness exercises out-of-order reception. Hash correctness is an abstraction;
the C++ tests compare incremental and whole-buffer hashes with a known vector
and an independent GLib computation, including when OpenSSL is selected.

The responsibilities and interface are:

| Layer | Owns | Exposes to the next layer |
|---|---|---|
| Data transfer | Immutable transfer binding, chunk scheduling/reassembly, bounds, duplicate checks, complete payload validation, abort tombstones and buffer lifetime | Validated payload receipt bound to the exact transfer, or transfer status |
| Object preparation | Payload restoration/schema checks and the final authority/history image | Target Prepared acknowledgement for the migration |
| Authority migration | Admission Close, read/write occupancy, Drain, Commit, Fence, activation and source retirement | The existing authority outcome |

Use `DataReady` for the logical transport receipt at this boundary, to distinguish
it from migration Prepared. The existing receiver model calls its sealed state
`Prepared`; that name alone grants no object authority and permits no activation.
The transfer state machine must neither release Lease permissions nor decide
Commit, reopen the source, activate the destination or delete source data.

Establish a validated, immutable binding before accepting chunks: transfer ID,
object identity, migration generation or immutable snapshot identity, authorized
peers, total length, chunk layout, schema and expected digest. Table metadata can
supply the migration binding, but both Hosts must actually validate and retain
it. Copy transfers also need a binding without an authority-migration decision.
The current frames retain the full authenticated ticket or snapshot reference;
the sender caches its encoding for the transfer. A future compact binding-ID
format must retain session authentication and prevent old frames from being
accepted for a new operation. Compact binding frames are not implemented here.

The transport model checks bounded outstanding work, out-of-order completion,
idempotent identical duplicates, rejection of conflicting duplicates, no readiness
with holes, identity isolation and no resurrection after Abort. The C++ receiver
also enforces total size/chunk bounds and schema restoration. A timeout remains
uncertainty, not cancellation; accepted callbacks retain their operation until
all accepted exchanges settle. Error completion is delivered once, and no new
chunks are submitted after the first failure.

`CaptureOverlap.tla` is a separate small composition model. It contains abstract
data readiness, read/write occupancy, capture lifetime and final history, with
no chunk details. Three mutants permit capture alongside a writer, drop the
capture pin before Abort, or prepare history before Drain; a witness reaches
capture while readers remain. These models remain bounded complementary
abstractions, not a machine-checked C++ refinement chain or a liveness proof.

### Overlap and CPU work

Before Close, the coordinator reserves both Hosts' budgets. Its extended target
reservation also establishes the data peer connection, keeping synchronous
connection setup outside the closed interval. Legacy three-argument target
reservations remain accepted; the extended form binds the source address/port
and rejects a changed endpoint on retry.

After Close, the Host checks every admitted operation's declared effect,
including queued work. If all are reads, an independent, lazily started snapshot
worker captures the immutable Dataset while the normal business lane drains.
Any queued or executing write prevents early capture. This is an explicit
Dataset backend guarantee about concurrent const reads, not an inference from a
C++ method name or a capability's label. The capture has its own real backend
pin: Freeze acknowledgement and Abort restoration wait for it as well as business
pins. No client timeout releases it. If optional prefetch cannot start, the
normal post-Drain capture remains available.

Final authority/request history is still encoded after complete Drain. This
implementation overlaps **Dataset capture**, not the entire network transfer,
with readers. Package construction and transfer follow the final history capture;
the all-or-nothing Commit/Fence/activation boundary stays unchanged. Extending
network overlap would require separating the immutable payload from final
control history and verifying that composition explicitly.

On little-endian hosts, packed Dataset words use bulk copies for encoding and
restoration; big-endian hosts retain explicit little-endian conversion. The
schema and binary bytes are unchanged. The receiver borrows `string_data()/string_size()` from the decoded value
only during its synchronous append, avoiding both a temporary string and byte
vector. The decoded reply and backing I/O payload remain alive during that copy.
The receiver hashes newly contiguous data as it arrives rather than rescanning
the buffer at Seal.

Source migration capture appends the Dataset encoding directly to its reserved,
unpublished CPMG buffer. Early read-only capture writes the same package prefix;
after Drain, the final authority image is appended and the complete package is
hashed. No full-size intermediate Dataset byte buffer is needed. Prefix storage
is never exposed as a completed export, and Abort still waits for the capture
pin. The binary layout, schema, digest and authority boundaries are unchanged.

`CAPSTAN_SHA256_BACKEND=AUTO` (the default) uses OpenSSL Crypto when available and
falls back to GLib otherwise. `OPENSSL` requires that dependency; `GLIB` forces the
fallback. Both use ordinary SHA-256 with the same expected digest. The selected
`CAPSTAN_SHA256_PROVIDER` is recorded in benchmark metadata. CI tests both providers,
including strict no-exception and sanitizer configurations; hashing does not add
an exception or alter transport authentication.

### Composing Lease permissions with Copy/Cut

`LeasePermission.tla` expresses the permission rules from Lease's
`ParticipantCountRefined.tla`: a live `rw` root supplies unique write authority,
`ro` roots and borrowed readers supply read authority, and a write requires an RW
root with no readers. The participant equation is `count = live_root + readers`.
A lone RO participant is still not a writer. The model retains Lease's legal-call
assumption that reader acquisition cannot reenter an active write expression.

`LeaseCopyCut.tla` composes these guards with Host pins and transaction decisions:

- Copy acquires a read participant from either initial root mode. Capture is
  queued behind prior Host work, then split into start/finish so a write during
  capture would be observable. Disconnect does not release the backend's borrow.
- Selecting Cut is inert. Submitting it requires unique RW authority and no
  local readers, seals the original facade, and moves custody into the migration
  operation without creating another participant. Previously accepted Host pins
  still have to Drain before Frozen.
- A Prepared ACK is evidence for a subsequent Commit, not the Commit itself.
  Fencing ends the original usable authority before the target's fresh RW lineage
  is activated. A lost activation ACK can leave the target active and the observer
  uncertain; source retirement still waits for acknowledgement.
- Abort requires the target tombstone ACK and completed Drain. It creates a fresh
  source authority at a new epoch. The old sealed facade never becomes usable again.
  Disconnect cannot reverse a Commit decision.

The source Lease participant count, Host backend pins, and observer connection
are distinct variables. This is a semantic composition, not a claim that one
intrusive counter spans machines or that the C++ implementation has been formally
refined to this model. Ending an authority lineage in the model does not assert
that every inert C++ handle/control block has been physically destroyed. The
existing migration model separately covers its capture worker pin; the receiver
and clipboard models retain chunk validation, clone identity and immutable
snapshot checks. Those models are complementary abstractions, not a machine-checked
refinement chain.

Nine mutants independently disable read exclusion, borrow retention, RW-only Cut,
reader exclusion at Cut, Drain, the Prepared ACK requirement, source fencing,
Commit stability, or the fresh Abort lineage. Each must fail its designated
invariant. Three checks deliberately assert false invariants against the correct
model to obtain paths reaching shared-RO Copy completion, committed activation
with a lost ACK, and fresh-authority Abort completion. They are reported as
`reachability`, separately from safety checks and faulty-protocol mutants; they
prove those bounded paths exist, not that every execution eventually completes.

```sh
ctest --test-dir build --output-on-failure
python3 scripts/check_tla.py --jar build-tla-download/tla2tools.jar
python3 scripts/check_tla.py --jar build-tla-download/tla2tools.jar --nightly
```

No WAL, restart recovery, automatic failover or cross-object atomic transaction
is provided. Start a fresh control session after a process-group restart. An
interactive system clipboard integration and a persistent location service are
outside this experimental CLI/API; the caller retains the explicit route and
session coordinator records.
