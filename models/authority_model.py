#!/usr/bin/env python3
"""Bounded authority-protocol model; Python 3.9+, standard library only.

One fixed object, two honest clients, immutable shared/exclusive epochs,
old capabilities retained forever, and at most N admitted operations.
Operations represent accepted/queued/executing work, not just CPU execution.

This deliberately does NOT model TCP, controller ACK/dedup transactions,
restarts, authentication, C++ memory ordering, callbacks, or liveness.
Installed grants are immediately available: delayed use is still possible
because clients can retain them and schedule an admission at any later step.

A mutation removes the empty-active-set requirement from Install. The tool
must find a counterexample for the mutation; otherwise the check fails.
"""
from __future__ import annotations
import argparse
import json
from collections import deque
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Optional

# Configuration = (mode, authorized clients). Client IDs: A=0, B=1.
CONFIGS = (
    ("None", ()), ("Shared", (0,)), ("Shared", (1,)),
    ("Shared", (0, 1)), ("Exclusive", (0,)), ("Exclusive", (1,)),
)
TARGET_ORDER = (4, 5, 3, 1, 2, 0)
# Token: (epoch, config, client). Operation: (epoch, config, client, write).
Token = tuple[int, int, int]
Operation = tuple[int, int, int, bool]

@dataclass(frozen=True)
class State:
    phase: str = "open"
    epoch: int = 0
    config: int = 0
    target: int = -1  # -1 absent; -2 destroy
    active: tuple[Operation, ...] = ()
    tokens: tuple[Token, ...] = ()


def authorized(s: State, token: Token, write: bool) -> bool:
    epoch, cfg, client = token
    if s.phase != "open" or epoch != s.epoch or cfg != s.config:
        return False
    mode, holders = CONFIGS[s.config]
    return client in holders and (not write or mode == "Exclusive")


def invariant(s: State) -> Optional[str]:
    if s.phase == "dead" and s.active:
        return "DestroyedWithActiveOperation"
    mode, holders = CONFIGS[s.config]
    for epoch, cfg, client, write in s.active:
        if epoch != s.epoch:
            return "ActiveEpochInvariant"
        if cfg != s.config or client not in holders:
            return "ActiveGrantInvariant"
        if write and mode != "Exclusive":
            return "WriteWithoutExclusive"
    for a in s.active:
        if a[3] and any(b[2] != a[2] for b in s.active):
            return "ConflictingActiveHolders"
    return None


def config_name(cfg: int) -> str:
    mode, holders = CONFIGS[cfg]
    return mode + "(" + ",".join("AB"[h] for h in holders) + ")"


def successors(s: State, max_epoch: int, max_inflight: int, unsafe: bool):
    if s.phase == "dead":
        return
    # Completion is actual remote completion, never a local timeout/cancel.
    for i, op in enumerate(s.active):
        if i and s.active[i - 1] == op:
            continue
        yield (f"Complete {'write' if op[3] else 'read'} by {'AB'[op[2]]} e{op[0]}",
               replace(s, active=s.active[:i] + s.active[i+1:]))
    if s.phase == "open":
        if len(s.active) < max_inflight:
            for token in s.tokens:
                for write in (False, True):
                    if authorized(s, token, write):
                        op = token + (write,)
                        yield (f"Admit {'write' if write else 'read'} by {'AB'[token[2]]} e{token[0]}",
                               replace(s, active=tuple(sorted(s.active + (op,)))))
        if s.epoch < max_epoch:
            for target in TARGET_ORDER:
                yield (f"Close for {config_name(target)} e{s.epoch+1}",
                       replace(s, phase="closing", target=target))
        yield ("Close for destruction", replace(s, phase="closing", target=-2))
    elif s.phase == "closing":
        if s.target == -2:
            if not s.active:  # The tested mutation affects Install only.
                yield ("Destroy after drain", replace(s, phase="dead", target=-1))
        elif not s.active or unsafe:
            epoch = s.epoch + 1
            new_tokens = tuple((epoch, s.target, h) for h in CONFIGS[s.target][1])
            yield (f"Install {config_name(s.target)} e{epoch}"
                   + (" WITHOUT DRAIN" if s.active else " after drain"),
                   replace(s, phase="open", epoch=epoch, config=s.target,
                           target=-1, tokens=s.tokens + new_tokens))


def trace_to(parent, bad: State):
    trace = []
    node = bad
    while parent[node] is not None:
        prev, label = parent[node]
        trace.append(label)
        node = prev
    trace.reverse()
    return trace


def explore(max_epoch: int, max_inflight: int, unsafe: bool) -> dict:
    start = State()
    queue = deque([start])
    parent = {start: None}
    edges = 0
    checked = 0
    rejected_stale = 0
    while queue:
        s = queue.popleft()
        checked += 1
        violation = invariant(s)
        if violation:
            return {"passed": False, "checked_states": checked,
                    "discovered_states": len(parent), "edges": edges,
                    "violation": violation, "trace": trace_to(parent, s)}
        for token in s.tokens:
            if token[0] < s.epoch:
                assert not authorized(s, token, False)
                assert not authorized(s, token, True)
                rejected_stale += 1
        for label, nxt in successors(s, max_epoch, max_inflight, unsafe):
            edges += 1
            if nxt not in parent:
                parent[nxt] = (s, label)
                queue.append(nxt)
    return {"passed": True, "checked_states": checked,
            "discovered_states": len(parent), "edges": edges,
            "stale_token_checks": rejected_stale, "violation": None, "trace": []}


def unit_checks() -> None:
    ro = State(epoch=1, config=3, tokens=((1, 3, 0), (1, 3, 1)))
    assert authorized(ro, (1, 3, 0), False)
    assert authorized(ro, (1, 3, 1), False)
    assert not authorized(ro, (1, 3, 0), True)
    assert not authorized(replace(ro, phase="closing", target=4), (1, 3, 0), False)
    rw = State(epoch=2, config=4, tokens=((2, 4, 0),))
    assert authorized(rw, (2, 4, 0), True)
    assert not authorized(rw, (2, 4, 1), True)
    assert not authorized(rw, (1, 4, 0), True)
    assert not authorized(replace(rw, phase="dead"), (2, 4, 0), True)
    assert invariant(State(phase="dead", active=((0, 0, 0, True),)))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--max-epoch", type=int, default=3, choices=range(1, 5))
    ap.add_argument("--max-inflight", type=int, default=2, choices=range(1, 4))
    ap.add_argument("--output", type=Path)
    args = ap.parse_args()
    if args.max_epoch < 2:
        ap.error("Use at least two epochs so the missing-drain mutation is exercised.")
    unit_checks()
    safe = explore(args.max_epoch, args.max_inflight, False)
    mutant = explore(args.max_epoch, args.max_inflight, True)
    report = {
        "model": "one-object authority safety, not an implementation proof",
        "bounds": {"clients": 2, "objects": 1, "max_epoch": args.max_epoch,
                   "max_admitted_operations": args.max_inflight},
        "normal_protocol": safe,
        "missing_drain_mutation": mutant,
        "limitations": ["No network ACK/dedup state", "No crash recovery/authentication",
                        "No C++ memory model or callback lifetime", "No liveness proof"],
        "overall_pass": safe["passed"] and not mutant["passed"],
    }
    text = json.dumps(report, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0 if report["overall_pass"] else 1

if __name__ == "__main__":
    raise SystemExit(main())
