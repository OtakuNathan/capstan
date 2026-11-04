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
    parser.add_argument('--nightly', action='store_true', help='Explore three epochs instead of two')
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
             ('EarlyAck', 'AckIsInstalled')]
    records = []
    for name, invariant in cases:
        meta = out / (name + '-states')
        command = ['java', '-Xmx' + args.heap, '-XX:+UseParallelGC', '-cp', str(jar),
                   'tlc2.TLC', '-workers', str(args.workers), '-metadir', str(meta),
                   '-config', str(ROOT / 'models/tla' / (name + '.cfg')),
                   str(ROOT / 'models/tla/Authority.tla')]
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
        records.append(dict(model=name, command=command, exit_code=code,
                            expected_counterexample=invariant, passed=passed,
                            seconds=round(time.monotonic()-start, 3),
                            states_generated=int(states[-1][0]) if states else None,
                            distinct_states=int(states[-1][1]) if states else None))
        print(f'{name}: exit={code}, expected={invariant or "safe"}, passed={passed}', flush=True)
    report = dict(passed=all(row['passed'] for row in records), tools_version=TOOLS_VERSION,
                  tools_sha256=TOOLS_SHA256, nightly=args.nightly, checks=records,
                  scope='Bounded single-Host/single-directory safety; no liveness, authentication, crash recovery or C++ memory-model proof.')
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
