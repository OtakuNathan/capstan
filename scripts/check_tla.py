#!/usr/bin/env python3
"""Run bounded authority safety checks and require counterexamples for mutants."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time
import urllib.request

TOOLS_VERSION = '1.7.4'
TOOLS_SHA256 = '936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88'
TOOLS_URL = f'https://github.com/tlaplus/tlaplus/releases/download/v{TOOLS_VERSION}/tla2tools.jar'
ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jar', type=Path, default=os.environ.get('TLA2TOOLS_JAR'))
    parser.add_argument('--download-tools', action='store_true')
    parser.add_argument('--nightly', action='store_true', help='Increase epoch, chunk, pin, Lease reader and value bounds')
    parser.add_argument('--suite', choices=['all','durable'], default='all', help='Select the complete suite or only the crash-recovery composition')
    parser.add_argument('--workers', type=int, default=2)
    parser.add_argument('--heap', default='512m')
    parser.add_argument('--timeout', type=int, default=600, help='Per model, seconds')
    parser.add_argument('--output', type=Path, default=ROOT / 'build-tla')
    args = parser.parse_args()
    if args.workers < 1 or args.timeout < 1:
        parser.error('workers and timeout must be positive')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    jar = Path(args.jar).resolve() if args.jar else out / 'tla2tools.jar'
    if not jar.exists() and args.download_tools:
        jar.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(TOOLS_URL, timeout=60) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != TOOLS_SHA256:
            raise SystemExit('Downloaded TLA+ tools digest mismatch')
        jar.write_bytes(data)
    if not jar.is_file():
        parser.error('Supply --jar, TLA2TOOLS_JAR, or --download-tools')
    if hashlib.sha256(jar.read_bytes()).hexdigest() != TOOLS_SHA256:
        raise SystemExit('TLA+ tools digest mismatch; expected pinned v' + TOOLS_VERSION)
    cases = [('AuthorityNightly' if args.nightly else 'Authority', None),
             ('MissingDrain', 'ActiveEpoch'),
             ('MissingPermission', 'AuthorizedOperations'),
             ('EarlyAck', 'AckIsInstalled'),
             ('DirectTransferNightly' if args.nightly else 'DirectTransfer', None),
             ('BypassSourceCheck', 'SourceCurrent'),
             ('DirectMissingDrain', 'ActiveEpoch'),
             ('EarlyClaim', 'ClaimCurrent'),
             ('TxKeyCollision', 'NamespaceIsolation'),
             ('DataTransferNightly' if args.nightly else 'DataTransfer', None),
             ('EarlySeal', 'PreparedComplete'),
             ('ObjectMigrationNightly' if args.nightly else 'ObjectMigration', None),
             ('MigrationMissingDrain', 'DrainBeforeFreeze'),
             ('EarlyActivation', 'ActivationFenced'),
             ('MigrationTimeoutReopens', 'CommittedSourceClosed'),
             ('MigrationRollback', 'ActivationCommitted'),
             ('MigrationDropsDedup', 'DedupPreserved'),
             ('MigrationCaptureReleasedEarly', 'CaptureProtected'),
             ('ImportReservation', None), ('ImportOpenDuringReceive', 'ReservedGate'),
             ('Clipboard', None), ('CloneUsesLiveData', 'CopyTimeValue'),
             ('CloneReusesIdentity', 'CloneIdentity'), ('CloneResurrectsAbort', 'AbortTerminal'),
             ('PlacementReplay', None), ('RetireLaterPlacement', 'ActiveData'), ('PublishOldPlacement', 'CurrentRoute'),
             ('LeaseCopyCutNightly' if args.nightly else 'LeaseCopyCut', None),
             ('LeaseCopyWriteWithReaders', 'WriteExclusion'),
             ('LeaseCopyDetachReleasesBorrow', 'CopyBorrowHeld'),
             ('LeaseCutReadOnly', 'CutRequiresWriteAuthority'),
             ('LeaseCutWithReaders', 'CutHasNoReaders'),
             ('LeaseCutMissingDrain', 'FrozenHasNoPins'),
             ('LeaseCommitBeforePrepared', 'CommitHasPreparedAck'),
             ('LeaseCutDuplicatesWriter', 'SingleWriteAuthority'),
             ('LeaseDisconnectRollsBack', 'CommitIsStable'),
             ('LeaseAbortReopensOld', 'SealedNeverReopens'),
             ('LeaseSharedCopyWitness', 'NoSharedCopyCompletion'),
             ('LeaseLostAckWitness', 'NoCommittedLostAck'),
             ('LeaseAbortWitness', 'NoFreshAbortCompletion'),
             ('WindowedDataTransferNightly' if args.nightly else 'WindowedDataTransfer', None),
             ('WindowUnbounded', 'WindowBound'), ('WindowWrongBinding', 'BindingIsolation'),
             ('WindowOverwriteDuplicate', 'DuplicatesPreserved'), ('WindowEarlyReady', 'ReadyComplete'),
             ('WindowAbortResurrect', 'AbortTerminal'), ('WindowHashOutOfOrder', 'HashedPrefix'),
             ('WindowReorderWitness', 'NoReordering'),
             ('CaptureOverlapNightly' if args.nightly else 'CaptureOverlap', None),
             ('CaptureAllowsWriter', 'CaptureExcludesWriters'), ('CaptureDropsPin', 'CaptureProtected'),
             ('CaptureEarlyHistory', 'CommittedImage'), ('CaptureOverlapWitness', 'NoOverlap'),
             ('DurableRPCNightly' if args.nightly else 'DurableRPC', None),
             ('DurableEarlyReply','AckSurvivesCrash'), ('DurableMissingReceipt','AtomicBusinessCommit'),
             ('DurableVolatileDedup','AtomicBusinessCommit'), ('DurablePayloadAlias','AckSurvivesCrash'),
             ('DurableLostIntent','IntentBeforeSideEffect'), ('DurableMissingDrain','DrainBeforeInstall'),
             ('DurableReopenRecovery','ClosedRecovery'), ('DurableEarlyInstallAck','InstallAckIsDurable'),
             ('DurableLostAckWitness','NoLostAckRecovery'), ('DurableGrantRecoveryWitness','NoGrantRecovery')]
    if args.suite=='durable':
        cases=[case for case in cases if case[0].startswith('Durable')]
    modules = {name: 'DataTransfer' for name in ('DataTransfer', 'DataTransferNightly', 'EarlySeal')}
    modules.update({name: 'ObjectMigration' for name in ('ObjectMigration', 'ObjectMigrationNightly',
        'MigrationMissingDrain', 'EarlyActivation', 'MigrationTimeoutReopens', 'MigrationRollback', 'MigrationDropsDedup', 'MigrationCaptureReleasedEarly')})
    modules.update({name: 'ImportReservation' for name in ('ImportReservation', 'ImportOpenDuringReceive')})
    modules.update({name: 'Clipboard' for name in ('Clipboard', 'CloneUsesLiveData', 'CloneReusesIdentity', 'CloneResurrectsAbort')})
    modules.update({name: 'PlacementReplay' for name in ('PlacementReplay', 'RetireLaterPlacement', 'PublishOldPlacement')})
    lease_cases = {name for name, _ in cases if name.startswith('Lease')}
    modules.update({name: 'LeaseCopyCut' for name in lease_cases})
    modules.update({name: 'WindowedDataTransfer' for name, _ in cases if name.startswith('Window')})
    modules.update({name: 'CaptureOverlap' for name, _ in cases if name.startswith('Capture')})
    modules.update({name: 'DurableRPC' for name, _ in cases if name.startswith('Durable')})
    witnesses = {'LeaseSharedCopyWitness', 'LeaseLostAckWitness', 'LeaseAbortWitness', 'WindowReorderWitness', 'CaptureOverlapWitness','DurableLostAckWitness','DurableGrantRecoveryWitness'}
    records = []
    for name, invariant in cases:
        meta = out / (name + '-states')
        command = ['java', '-Xmx' + args.heap, '-XX:+UseParallelGC', '-cp', str(jar),
                   'tlc2.TLC', '-workers', str(args.workers), '-metadir', str(meta),
                   '-config', str(ROOT / 'models/tla' / (name + '.cfg')),
                   str(ROOT / 'models/tla' / (modules[name] + '.tla' if name in modules else 'Authority.tla' if name in ('Authority','AuthorityNightly','MissingDrain','MissingPermission','EarlyAck') else 'DirectTransfer.tla'))]
        start = time.monotonic()
        log = out / (name + '.log')
        with log.open('w') as stream:
            try:
                code = subprocess.run(command, cwd=ROOT, stdout=stream,
                                      stderr=subprocess.STDOUT, timeout=args.timeout).returncode
            except subprocess.TimeoutExpired:
                code = 124
        text = log.read_text()
        if invariant:
            passed = code == 12 and f'Invariant {invariant} is violated' in text
        else:
            passed = code == 0 and 'Model checking completed. No error has been found.' in text
        states = re.findall(r'(\d+) states generated, (\d+) distinct states found', text)
        records.append(dict(model=name, kind='reachability' if name in witnesses else 'mutant' if invariant else 'safety',
                            command=command, exit_code=code,
                            expected_counterexample=invariant, passed=passed,
                            seconds=round(time.monotonic()-start, 3),
                            states_generated=int(states[-1][0]) if states else None,
                            distinct_states=int(states[-1][1]) if states else None))
        print(f'{name}: kind={records[-1]["kind"]}, exit={code}, expected={invariant or "safe"}, passed={passed}', flush=True)
    report = dict(passed=all(row['passed'] for row in records), tools_version=TOOLS_VERSION,
                  tools_sha256=TOOLS_SHA256, nightly=args.nightly, suite=args.suite, checks=records,
                  scope='Bounded authority, data receiver, clipboard, Lease permissions, placement and migration-decision safety with bounded reachability witnesses; DurableRPC additionally checks bounded process/power-loss recovery under atomic flushed-store assumptions. No authentication, device flush correctness, general liveness or C++ memory-model proof.')
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
