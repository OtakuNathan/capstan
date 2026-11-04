# Authority and failure semantics

Each object has one authoritative Host. The directory is a single instance with
one control session. The implementation does not resolve split brain or let a
fresh empty directory take over a live Host.

A grant binds full object identity, epoch, grant ID, authenticated holder, mode
and an OS-generated opaque token. Host validates its installed grant record and
the declared method effect. Client-supplied mode is not authority. Shared readers
can coexist; exclusive authority has one holder. All work is serialized per object.

## Transition sequence

1. **Close:** under the same decision lock as RPC admission, close the old gate
   and record the desired transaction. Reserve transition state before committing.
2. **Drain:** all accepted, queued and executing old operations finish. A client
   timeout, cancel or disconnect cannot decrement Host in-flight.
3. **Install:** only with no old operations remaining, install the new immutable
   epoch/grants or a tombstone. Destroy the resource only after that barrier.
4. **Ack:** complete the pending control RPC after installation. Bounded deferred
   completions handle inline/reentrant completion and control-client loss.
5. **Grant:** Table validates the matching transaction result before publishing
   capabilities. There is no periodic ACK polling timer on the normal path.

Control transaction IDs support idempotent retry/query. Duplicate messages must
not repeat installation; identical IDs with different payloads are rejected.
Old messages cannot roll back an epoch. An uncertain control result can leave
Table pending indefinitely rather than grant conflicting authority. There is no
automatic reconnection/recovery protocol; establish a new lab session after
stopping the old group. Dedup history is bounded and does not silently evict old
records to make unsafe progress.

## Business outcomes

| Outcome | Meaning |
|---|---|
| NotSubmitted | Local submission did not enter the transport path |
| Rejected | The request was explicitly refused before business execution |
| Completed | Remote execution produced a definitive result |
| OutcomeUnknown | Local observation cannot establish execution outcome |

A missing response does not imply the write did not occur. Capstan does not
retry side effects automatically or reclaim authority because a client deadline
expired. Closing a local object only performs local cleanup or schedules a best
effort action; explicit close completion is required to confirm remote release.

## Networking and bounded resources

Linux GIO handles nonblocking TCP with bounded frames, input/output queues,
sessions, request registrations and dedup records. Protocol errors, wrong arity,
wrong types, incorrect identity, unauthorized effects and out-of-range operations
use checked error paths. General allocator exhaustion without exceptions is a
process failure, not a successful protocol rollback.

Listeners bind loopback by default. Demo roles use private files holding opaque
OS-generated credentials. This is a trusted-lab assumption, not a cryptographic
protocol: there is no TLS, hostile-controller defense or public-network security
claim. No firewall, service installation or automatic host discovery is performed.

Compile-time access constraints, deterministic transition tests, callback races,
real TCP fault scenarios and the bounded formal model each test different parts
of these guarantees. Their limits are documented separately.
