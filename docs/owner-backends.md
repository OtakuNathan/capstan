# Logical owners and storage backends

An owner represents one logical object and its authority boundary. Its storage
may be one in-process allocation, several private copies, or an adapter to an
external database or storage group. Physical replica count does not create
additional public object identities or business write grants.

Capstan's deterministic authority state machine remains independent of storage.
Lease governs local access; Dynabridge supplies the advertised typed calls;
FF schedules accepted work. The owner backend supplies the object implementation
and snapshot policy. The current wire method/effect declarations remain the
Dataset example's contract. Adding business methods still requires explicit
schema and effect declarations; storage policy never infers permissions.

## Extension point

[`owner_backend_contract<Backend>`](../include/capstan/owner/backend.hpp) checks a
C++14 structural interface. No base class, virtual dispatch or callable erasure
is required. Backend state is injected through a retained `shared_ptr<Backend>`;
each logical object has its own `unique_ptr<Backend::object_type>`. That unique
handle can itself retain a shared database client or an entire storage group.

| Backend operation | Contract |
|---|---|
| `create(key, bytes, seed)` | Return an owning object handle for initial demo creation. A null result fails startup. |
| `restore(key, owner_snapshot_view, out)` | Validate backend snapshot bytes and stage an isolated object; return `status`. Success owns all required input storage. |
| `restore_reservation(max_snapshot_bytes)` | A conservative byte bound for any staged object restored from a snapshot up to this size, including private copies. Must be monotone and `noexcept`. |
| `can_inline(method)` | Explicitly opt into bounded, nonblocking control-thread execution. The current demo only considers `version`; return false for database/network calls. Must be `noexcept`. |
| `concurrent_read_capture` | Explicitly promise that snapshot reads can overlap admitted business reads. Set false unless the storage implementation supports this. |

The object handle supplies:

| Object operation | Contract |
|---|---|
| `try_dispatch(method, grant, args, result)` | Execute the advertised business operation under an admitted Host pin and serial object lane. |
| `capture_snapshot(out)` | Produce an owning, immutable snapshot under a read/capture pin. |
| `append_snapshot(out)` | Append the same snapshot representation to an unpublished migration package. |
| `snapshot_bytes()` | Exact encoded snapshot size for the currently pinned version; positive and `noexcept`. |
| `resource_bytes()` | Conservative Host-budget charge for the object's retained resources; include private local copies. Must be `noexcept`. |

Sizing and placement hints are bounded, nonblocking metadata queries and must be
safe while business work runs on another thread. Snapshot size is stable for a
pinned capture; growth between pre-Close reservation and capture can reject the
export rather than exceed its reservation. External storage budgets remain the
backend's responsibility.

This adapter uses **synchronous backend completion**: returning from dispatch or
capture means the operation has finished accessing its borrowed resources. The
Host then retires its real pin before delivering completion. A backend must not
return an observer timeout while leaving untracked work using those resources.
External clients need their own completion/fencing mechanism to satisfy this
contract. Merely forwarding a request to another machine is insufficient.

Business dispatch runs on the FF worker unless bounded inline execution is
explicitly allowed. Creation and restore currently run on the Host context and
must not wait for callbacks requiring that same context. A fully asynchronous
backend completion adapter would be a separate extension with its own pin and
shutdown contract.

Restore prepares private state without publishing new write authority. The
Host stages authority/history, installs the owning handle and opens admission
only through the existing activation protocol. Releasing an unpublished handle
must safely discard its staging state; destroying a local handle need not mean
deleting a shared external database. Backend cleanup policy controls that choice.
Backend/context lifetime exceeds both active and unpublished restored objects.
The demo Host/registry itself still has process lifetime.

## Default Host is a demo implementation

[`single_process_owner_backend`](../examples/owner/single_process.hpp) creates the
existing in-memory Dataset binding. `capstan_host` selects this policy explicitly:

```cpp
using host_runtime =
    basic_host_runtime<capstan::examples::single_process_owner_backend>;
using host_registry = basic_host_registry<host_runtime>;
```

The templated Host integration lives in
[`src/apps/owner_host.hpp`](../src/apps/owner_host.hpp); registry slots inherit
their root's retained backend context. Copy/Paste and migration restore through
the selected policy, including dynamically routed objects. CPMG still carries
the same envelope and authority history, but its snapshot payload is opaque to
Host. The default CPSN payload remains byte-compatible with existing captures.
Backends exchanging snapshots must agree on their schema or implement explicit
conversion in restore; there is no automatic codec negotiation.

A custom frontend can instantiate the same integration with another policy:

```cpp
auto storage = std::make_shared<MyOwnerBackend>(database_context);
basic_host_runtime<MyOwnerBackend> runtime(config, context, bytes, 0,
                                          history_limit, storage);
basic_host_registry<decltype(runtime)> objects(runtime);
```

This changes storage/execution policy while preserving one authority domain.
It does not replicate Capstan's own control state, add restart recovery, or make
a single control frontend automatically highly available. An existing redundant
database can satisfy storage guarantees internally without exposing its replicas
as separate Capstan owners.

## Multiple copies behind one owner

[`mirrored_memory_owner_backend`](../examples/owner/mirrored_memory.hpp) is a
second example with two private Dataset copies, one public identity, a retained
backend context and an `RPL1` snapshot prefix. Its Host cannot decode that payload
as CPSN; restore goes through the backend. It declines inline execution and
read/capture overlap. It reserves space for both copies and validates both before
performing the allocation-free in-memory mutation.

Both copies live in one process; the example demonstrates the abstraction, not
network replication or crash-atomic commit. The real TCP clipboard test exercises
both backends through the same Copy, independent clone Paste, write, migration,
deduplication, Abort/late-reply and aggregate-quota scenarios. It also checks
replica agreement, backend-context inheritance and final object cleanup.

## Exception policy and verification

Strict no-exception builds require `noexcept` on the actual backend
create/restore and object dispatch/capture/append hooks. Relaxed mode permits
unannotated hooks; it provides no exception recovery in a no-exception build.
Sizing, placement hints and destructors must be nonthrowing in every mode.
Standard allocation failure remains process-fatal without exceptions.

`noexception_policy_matrix` includes annotated/unannotated owner backends under
strict and relaxed builds. `host_execution_test` verifies that a backend's refusal
of inline execution wins over the demo's global inline-version setting.

No new authority or transfer phase is introduced. Existing Drain, activation,
capture-pin and Abort models still apply under the backend's completion contract;
this refactoring does not prove a database's internal replication algorithm.
