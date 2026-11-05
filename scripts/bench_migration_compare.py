#!/usr/bin/env python3
"""Alternate two existing builds to compare clipboard/migration latency and RSS."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from bench_migration import stats, summarize


def hashes(build):
    return {name: hashlib.sha256((build / name).read_bytes()).hexdigest()
            for name in ('capstan_host', 'capstan_migration_demo')}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sizes-mib', type=int, nargs='+', default=[1, 8, 32, 64])
    parser.add_argument('--rounds', type=int, default=5)
    parser.add_argument('--profile', action='store_true', help='Record opt-in phase CPU clocks; includes instrumentation overhead.')
    parser.add_argument('--pump-mode', choices=['poll', 'blocking'], default='poll')
    args = parser.parse_args()
    if args.rounds < 2 or len(set(args.sizes_mib)) != len(args.sizes_mib) or any(n < 1 or n > 64 for n in args.sizes_mib):
        parser.error('Use at least two rounds and distinct sizes in 1..64 MiB.')
    builds = dict(before=args.before.resolve(), after=args.after.resolve())
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    reports = {label: dict(binary_sha256=hashes(build), samples=[]) for label, build in builds.items()}
    order = []
    for iteration in range(-1, args.rounds):
        offset = max(iteration, 0) % len(args.sizes_mib)
        sizes = args.sizes_mib[offset:] + args.sizes_mib[:offset]
        labels = ('before', 'after') if iteration % 2 == 0 else ('after', 'before')
        for size in sizes:
            for label in labels:
                name = f'{"warmup" if iteration < 0 else iteration + 1}-{size}m-{label}'
                destination = output / name
                result = subprocess.run([sys.executable, str(Path(__file__).with_name('run_migration_demo.py')),
                    '--build-dir', str(builds[label]), '--output', str(destination), '--bytes', str(size * 1048576),
                    '--pump-mode', args.pump_mode] + (['--profile'] if args.profile else []),
                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                if result.returncode:
                    raise RuntimeError(f'{name}: {result.stdout}')
                row = json.loads((destination / 'report.json').read_text())
                if not row['passed'] or len(row['phase_seconds']) != 3:
                    raise RuntimeError(f'{name}: missing validation or phase timing')
                environment = row.pop('environment')
                if reports[label].setdefault('environment', environment) != environment:
                    raise RuntimeError('Environment changed during comparison')
                row.pop('scope')
                if iteration >= 0:
                    reports[label]['samples'].append(dict(round=iteration + 1, **row))
                    order.append(name)
                print(f'{name}: Cut {row["end_to_end_seconds"] * 1000:.2f} ms', flush=True)
    for label, report in reports.items():
        if report['binary_sha256'] != hashes(builds[label]):
            raise RuntimeError(f'{label} binaries changed during comparison')
        report.update(passed=True, rounds=args.rounds, discarded_runs_per_size=1,
                      summary=summarize(report['samples'], args.sizes_mib))
    paired = []
    for size in args.sizes_mib:
        rows = {label: [r for r in report['samples'] if r['native_bytes'] == size * 1048576]
                for label, report in reports.items()}
        paired.append(dict(native_mib=size, cut_before_over_after=stats([
            before['end_to_end_seconds'] / after['end_to_end_seconds']
            for before, after in zip(rows['before'], rows['after'])])))
    report = dict(passed=True, captured_utc=datetime.now(timezone.utc).isoformat(), builds=reports,
        instrumentation=args.profile, pump_mode=args.pump_mode,
        measured_order=order, paired=paired,
        scope='Alternating build order each round, rotating sizes, fresh loopback processes; no saturation or p99 claim.',
        timing='Cut includes reservations, Drain, capture, transfer, activation and cleanup. Query/validation RPCs are outside phase timers. Selected demo pump behavior and optional profiling overhead are included.',
        memory='VmHWM covers the whole demo including two retained clones, not incremental migration storage.')
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    for old, new in zip(reports['before']['summary'], reports['after']['summary']):
        print(f'{old["native_mib"]} MiB: {old["cut_ms"]["median"]:.2f} -> {new["cut_ms"]["median"]:.2f} ms, '
              f'{new["cut_package_mib_per_second"]["median"]:.2f} MiB/s')


if __name__ == '__main__':
    main()
