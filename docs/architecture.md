# Architecture and ownership

Capstan composes three libraries rather than replacing their responsibilities.
One logical object is served through one authoritative owner domain; its storage
implementation is selected by an [owner backend](owner-backends.md). The default
Host executable uses the single-process Dataset backend as a demo. A backend
may retain multiple private copies or wrap an external storage group. A Table is a directory
and control-plane coordinator, not a data-plane hop. Warm business calls reuse a
grant and contact Host directly.

The demo creates one Dataset of 64 MiB. `version()` returns a scalar;
`summary()` scans local storage and returns a small result; `add_range()` mutates
a bounded range under exclusive authority. A holder can transfer its exclusive
authority directly to another authenticated holder, who claims its successor from
Host. Table snapshots Host for fetch/management and never supplies cached authority.
A control-plane transition can change
readers, transfer exclusive authority, revoke access or install a tombstone.

## Typed access

`contracts/` declares import/export operations and method effects. The Capstan
Portal adapter implements a real object channel with session/incarnation/object
identity; the generic scalar RPC backend is not assumed to supply remote object
lifetime automatically. Native dispatch checks the receiver identity and arguments.
A full grant also binds epoch, holder, grant identifier, access mode and opaque token.

Lease provides local access constraints. A shared facade exposes readonly methods;
exclusive handles cannot be copied. The generated Portal imports are not
const, so the adapter presents a deliberate readonly interface without const_cast
or relabeling remote writes as reads.

Capability backing owns stable context/facade storage. Async operation frames
retain the backing and a real Lease borrow/token until completion. Copying a local
shared handle does not create a remote control transaction for each copy. Network
acquire/close is explicit and never hidden in topology constructors, moves,
operator-> or destructors.

## Async execution

The local request path is encode → submit asynchronous GIO I/O → FF external
awaitable completion → checked decode. The external adapter's operation state
outlives the public waiter where needed. Exactly one terminal callback is forwarded;
submission rejection produces no later callback. Inline completion is supported.
After a callback can release its awaitable, code must not touch that awaitable.

Client timeout adapters can detach a waiter from late transport completion.
Transport state remains independently owned. Host pins remain independent of
client handle lifetime and are released only on actual remote completion.

Business work runs in one serial lane per object. The optional FF libuv executor
uses a dedicated caller-owned loop, a bounded preallocated task ring and explicit
retained-task rejection. It is not libuv's global thread pool. Close drains accepted
tasks and closes its handle asynchronously. No queue allocation occurs in dispatch.
The default alternative is FF's simple executor. Network I/O remains GIO in both.

An outer business blueprint may await RPC operations whose inner local blueprints
encode, submit, await and decode. This preserves the same local-executor boundary;
it does not turn remote Hosts into FF executors. Capstan's current demo is not a
generic distributed closure or arbitrary workflow migration framework.

## Source map

| Directory | Purpose |
|---|---|
| `include/capstan/authority` | Deterministic Host/directory state machines |
| `include/capstan/access` | Lease-backed capabilities and local ownership |
| `include/capstan/owner`, `examples/owner` | Structural owner backend contract and single-process/mirrored storage examples |
| `include/capstan/bridge` | Portal typed object/codec integration |
| `include/capstan/async` | FF request/completion and detachable waiters |
| `include/capstan/transport`, `src/transport` | Bounded nonblocking GIO framing |
| `src/apps` | Host, Table, clients, business worker and ACK completion adapter |
| `tests` | Compile probes/negatives, state machine, callback and TCP tests |
| `models` | Independent bounded Python and TLA+ safety models |
| `bench/latest` | Published baseline measurements |
| `bench/direct-transfer/latest` | Direct-transfer measurements and validation |

The direct-transfer SDK seals at submission, retains bounded source tombstones,
and permits existing Lease guards to finish. Its separate control connection
keeps transfer admission independent of pending business I/O. See
[direct transfer](direct-transfer.md).
