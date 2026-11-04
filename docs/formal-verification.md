# Bounded TLA+ verification

The specification in `models/tla/Authority.tla` models one authoritative Host,
one directory control session, two clients, immutable shared/exclusive grants,
and a serial execution lane. The normal configuration explores two epochs and
two admitted operations; the nightly configuration explores three epochs.
An admitted operation occupies a queue slot while accepted, queued or executing.
Client abandonment does not complete or remove it.

The actions separate directory request, Host admission close, drain, installation,
ACK availability and directory grant publication. Sent control records remain
available for arbitrary delayed/duplicate delivery and retries. Old ACKs cannot
publish a different transaction. Control-session loss can prevent progress while
already accepted Host work completes. Issued tokens are retained so old epochs
remain available for attempted use after a transition.

Checked invariants:

- Every active operation belongs to the installed epoch and grant configuration.
- Writes require an installed exclusive grant; caller-supplied intent is insufficient.
- Destruction requires an empty queue, no executing operation, and closed admission.
- Only the queue head executes, and transition-pending admission stays closed.
- An ACK names an installed transaction; published grants match installed history.

The immutable installed-history domain also checks that epochs cannot roll back.
`CHECK_DEADLOCK FALSE` is intentional: a bounded terminal state or disconnected
session may stop making progress. This is a safety specification, with no fairness
or liveness claim. A retained message set represents possible delivery, not a
reliable network or automatic retry implementation.

Run with an existing pinned tool jar:

```sh
python3 scripts/check_tla.py --jar /path/to/tla2tools.jar --nightly
```

Or explicitly download the pinned tool:

```sh
python3 scripts/check_tla.py --download-tools --nightly
```

The runner requires TLA+ tools **v1.7.4**, SHA256
`936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88`.
Java 21 was used locally. The jar is not vendored. `--workers`, `--heap`, and
`--timeout` bound checker resources; default output is `build-tla/`.

The command also checks `DirectTransfer.tla` and requires seven intentional
counterexamples across both models:

| Mutant | Broken rule | Required violated invariant |
|---|---|---|
| MissingDrain | Install while old operations are active | ActiveEpoch |
| MissingPermission | Admit shared writes | AuthorizedOperations |
| EarlyAck | Produce ACK before installation | AckIsInstalled |
| BypassSourceCheck | Accept a stale holder transfer | SourceCurrent |
| DirectMissingDrain | Install a direct transfer before drain | ActiveEpoch |
| EarlyClaim | Issue before installation/currentness | ClaimCurrent |
| TxKeyCollision | Alias directory and holder transaction keys | NamespaceIsolation |

A mutant passes the harness only when TLC exits 12 with the expected invariant
violation. A syntax error, tool crash, timeout, or unexpectedly safe mutant fails.
The nightly workflow runs at **02:23 UTC**, also supports manual dispatch, and
uploads logs plus the machine-readable report. It does not run on every push.

Local nightly-bound verification explored **43,018 distinct states / 356,003
generated states**, exhausted the queue and found no safety error. All three
mutants produced the expected counterexample. See `bench/latest/verification.json`
for recorded results. TLC fingerprint-based exploration is bounded evidence,
not an implementation proof.

## Relationship to the implementation

`authority/host.hpp` supplies the shared serialization point for admission and
transition decisions. `authority_replies.hpp` holds bounded completion waiters
until actual install. Table observes current Host state rather than republishing historical ACK grants.
The model queue corresponds to all Host pins, not just currently running work.
The Python model in `models/authority_model.py` remains a smaller independent
sanity check run by ordinary CTest; it does not model ACKs.

The TLA+ model does not establish cryptographic identity, protection against a
malicious controller, conflicting same-ID payload handling, dedup capacity,
TCP framing, allocator behavior, C++ memory ordering, callback lifetime, crash
recovery, split-brain safety, or unbounded liveness. Deterministic C++ tests,
compile-negative tests, sanitizers and real TCP fault tests provide separate
implementation evidence. No cross-machine validation is claimed.

## Holder-transfer model

`DirectTransfer.tla` explores directory and holder competition over one Close /
Drain / Install slot, retained old source grants, origin/issuer/sequence keys,
accepted and installed transaction histories, historical receipts, current-only
claim and monotonic directory observations. The normal bound is two epochs;
nightly explores three epochs, two holders and two sequence numbers per origin.
The direct model represents at most one active old pin; the original model and
C++ queue tests cover multiple queued/executing pins. Namespaced collisions are
checked as a functional isolation property, not as a liveness proof.

The latest nightly direct model explored **19,292 distinct / 219,609 generated
states** with no safety violation. Its four mutants each exited 12 with the
expected invariant; the original model and its three mutants also passed the
harness. Logs and exact commands are in `bench/direct-transfer/latest/tla`.
This model does not prove credential secrecy, callback safety, bounded allocation
behavior or the C++ implementation. The directory publication variable records
snapshot observations, not a promise that a grant remains current after delivery.
