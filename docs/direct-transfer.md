# Holder-initiated direct exclusive transfer

Capstan supports transferring a current exclusive grant directly from its holder
A to an authenticated recipient B. The resource stays on Host. This change is
based on `92008504dc38246b5bc0549a9f674e9ed610a7c2`; the dependency lock is unchanged.

## Protocol and decision boundary

Host is the sole authority decision point. Table administration and holder
transfers share one mutex, transition slot, history bound and Close → Drain →
Install implementation. A transfer is a control request, never a business pin.
Both queued and executing business pins must finish before installation. There
is no ACK polling timer on the completion path.

Transactions are identified by `(origin, issuer, sequence)`. Directory issuer is
its established control session; holder issuer is its authenticated identity.
After authentication, accepted-history lookup precedes source-current validation.
An identical retry observes the same transaction; a changed payload is Conflict.
A new request requires the exact current exclusive grant. The lab's registered
business holders are 1 and 2; zero, self, unknown identities and control role 3
are not valid recipients. The recipient may be offline.

Host prepares successor credentials, history storage, staged grants and all
source reply encodings before Close. The source receives a secret-free receipt:

```
A -- transfer(source, B, transaction) --> Host
                 Close → Drain → Install(B, epoch + 1)
A <-- Installed receipt ---------------- Host
B -- claim(reference), authenticated B -> Host
B <-- current exclusive grant ---------- Host
```

A transfer reference contains object identity, transaction identity, source and
recipient holders and source epoch. It is a query locator, not authority. Only B
can claim; Host checks exact installed successor identity and Open/current state
again. `Installed + Superseded`, `Installed + Closed` and `Installed + Dead` are
valid historical observations, but cannot produce an old usable grant.

Transport outcomes remain distinct from transaction observations. Unknown and
NotSeen never prove rollback: the original request may arrive later. An accepted
transfer continues after A disconnects; an offline B does not restore A. If work
never completes, Drain can remain pending indefinitely.

## Local SDK and lifecycle

Include `capstan/access/transfer.hpp`. Preparation is local and inert:

```cpp
capstan::status code;
auto ticket = capstan::transfer_plan::prepare(source, recipient, sequence, code);
if (ticket) {
    auto reference = ticket->request().reference(); // safe to share; no token
    ticket->start(host_control, on_transfer_reply); // asynchronous FF path
}
```

`start` seals the source only when FF reaches endpoint submission. FF context or
result allocation failure before this boundary leaves it Live. After a submission
attempt, it remains sealed even on NotSubmitted, Rejected, Unknown or NotSeen.
`sealed()` distinguishes those cases. `retry` explicitly resubmits the exact
original request; it does not generate a transaction or restore business access.
`query_transfer` returns a checked receipt; `claim_transfer` checks the returned
grant's identity/epoch/recipient and materializes a real exclusive capability.

Seal uses the same ownership mutex as borrow/write creation. Existing guards
continue to own their Lease backing. Their destructors never reopen the source.
An invocation created before seal but delivered later is still subject to Host
admission order. A ticket owns independent state and can outlive the public source
wrapper; inline completion may destroy public handles safely. Endpoint lifetime
must cover its submissions, as with the existing I/O API. Callbacks do not execute
under the Host decision mutex. Fast runner still has no runner cancellation API.

The local registry preserves sealed-grant tombstones after all wrappers disappear.
It permits at most 64 live exclusive sessions and 4096 total retained entries;
capacity exhaustion fails closed, with no silent eviction. Host history is bounded
at 4096 decisions and the ACK adapter at 64 waiters. Claim before installation
returns Pending immediately, without allocating a waiter. Ordinary protocol,
capacity and nothrow FF allocation errors remain checked; general standard
allocator exhaustion without exceptions remains process-fatal.

The demo client uses a separate reusable Host control connection because GIO
permits one pending request per connection. A transfer can therefore reach Host
while a business RPC is in flight. No transport multiplexing or new scheduler was
introduced. There is no automatic local authority restoration/recovery API.

## Table compatibility

Table has no authoritative published-grant cache. Fetch queries Host and filters
the snapshot by authenticated caller. Configure, release and destroy use a fresh
Host snapshot and that exact expected epoch. Release additionally requires the
full old grant to match the snapshot. Stale/Busy rejection ends that decision;
Table never rebases it automatically. A subsequent explicit command is a new
decision and may deliberately use the new state.

A matching ACK with a Completed envelope establishes installation history.
Unknown/rejected envelopes cannot establish successful installation or a current
authority snapshot, even if their payload looks valid. Table observes Host again before
reporting current grants, and never rolls back its observed epoch on a late ACK.
As with any distributed grant, a concurrent subsequent transition may fence a
grant after the snapshot; every business admission still checks Host authority.

A definitive rejection clears the matching pending command. Unknown retains it.
The lab `reconcile` command queries the same transaction; `retry_admin` resubmits
its exact immutable target. Neither rebases. A failed/rejected retry cannot clear uncertainty about the
earlier attempt (a full ACK adapter can reject a duplicate of an accepted
transaction). These commands require a usable
control connection; GIO does not reconnect a broken session automatically. A
fresh empty Table with a different control session cannot take over a live Host.

## Wire compatibility

The transition schema is 1801; the old schema 801 is explicitly rejected. Transfer
reference, request, receipt and authority snapshot use schemas 1802–1805. Control
methods 3100–3104 implement transfer, query, claim, administrative snapshot and
caller-filtered grant fetch. Existing numeric statuses retain their values;
NotSeen and Superseded are appended. No source receipt contains successor secrets.

## Reproduction and evidence

For a narrated business example, run
`python3 scripts/run_direct_demo.py --build-dir build-strict`.
The [demo walkthrough](direct-transfer-demo.md) follows A updating the Dataset,
transferring during an in-flight summary, B claiming and updating it, then handing
it back to A while Table is stopped. The fault suite below remains separate.

```sh
ctest --test-dir build-strict --output-on-failure
python3 scripts/run_direct_transfer.py --build-dir build-strict --output build-strict/direct
python3 scripts/check_tla.py --jar /path/to/tla2tools.jar --nightly
python3 scripts/bench_direct_transfer.py --build-dir build-fast-strict \
  --output build-fast-strict/direct-benchmark --samples 100
```

The TCP test first confirms actual Host admission, then requires a Pending
transfer on a separate connection. It checks early claim rejection, old grant
fencing, Table SIGSTOP during A→B→A, unchanged directory counters, resource
summary preservation, stale release, superseded claim, source disconnect before
ACK, offline recipient and authenticated reconnection. Injected delays are used
only for fault orchestration. The benchmark uses zero injected delay.

| Obligations | Evidence |
|---|---|
| T01–T07, T10–T12, T14, T18 | `direct_transfer_test`: identity, permissions, dedup, origins, pending/installed and actual mutex races |
| T08–T09, T13, T19, T22, T24 | `direct_transfer_integration_test`: real codec/FF/Lease, delayed request, Unknown/duplicate callback, guards, typed claim |
| T02, T08, T11–T13, T16–T18 | `tcp_direct_transfer`: independent processes, connections and fault scenarios |
| T15 | directory observation/late-ACK unit test; transaction-specific historical ACK adapter test; `table_observation_test` for uncertain retries and replayed ACKs |
| T20–T21 | allocation tests, throwing-allocation sweep, waiter/history bounds, sealed registry exhaustion, reserved transaction limit |
| T23 | checked codec tests and authenticated TCP malformed-control probe, followed by valid requests |
| T24 | callback reentrancy and inline adapter destruction; ASan/UBSan and TSan selection |

The models and tests have distinct scopes. Real TCP covers disconnect-induced
lost ACK; deterministic loopback covers an Unknown callback followed by duplicate
completion and query-before-request arrival. The late-ACK/old-snapshot adversarial
orders are deterministic state-machine tests, not a packet-reordering claim.
Epoch overflow is guarded in code and existing authority tests; no exhaustive
64-bit state exploration is claimed. Hardware faults, hostile-network security,
crash persistence, partition recovery, HA, shared delegation, object migration and
cross-machine RPC remain outside this version.

Actual measurements and build results are recorded in
[`bench/direct-transfer/latest`](../bench/direct-transfer/latest/README.md).
The comparison labels Table administrative reassignment separately from
conditional holder transfer. Recipient-ready always includes B's claim/fetch.
Release CI runs the full suite, including both demos. Nightly CI runs both
authority models and all seven required mutation counterexamples.
