#!/usr/bin/env python3
"""Separate real Drain, data processing and demo polling with wall/CPU spans."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from bench_migration import stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bytes', type=int, default=64 * 1048576)
    parser.add_argument('--rounds', type=int, default=5)
    args = parser.parse_args()
    if args.rounds < 2:
        parser.error('Use at least two measured rounds.')
    build, output = args.build_dir.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binaries = {name: hashlib.sha256((build / name).read_bytes()).hexdigest()
                for name in ('capstan_host', 'capstan_migration_demo')}
    workloads = [('idle', 'none', 0, 1), ('read_hold', 'read', 100, 1),
                 ('write_hold', 'write', 100, 1), ('queued_reads', 'read', 25, 4)]
    cases = [(name, kind, delay, count, mode) for name, kind, delay, count in workloads
             for mode in ('poll', 'blocking')]
    rows, environment = [], None
    for iteration in range(-1, args.rounds):
        offset = max(iteration, 0) % len(cases)
        for name, kind, delay, count, mode in cases[offset:] + cases[:offset]:
            label = f'{"warmup" if iteration < 0 else iteration + 1}-{name}-{mode}'
            dest = output / label
            result = subprocess.run([sys.executable, str(Path(__file__).with_name('run_migration_demo.py')),
                '--build-dir', str(build), '--output', str(dest), '--bytes', str(args.bytes), '--profile',
                '--inflight', kind, '--inflight-count', str(count), '--work-delay-ms', str(delay),
                '--pump-mode', mode], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if result.returncode:
                raise RuntimeError(f'{label}: {result.stdout}')
            report = json.loads((dest / 'report.json').read_text())
            if environment is None:
                environment = report['environment']
            if report['environment'] != environment or not report['passed']:
                raise RuntimeError('Build changed or correctness checks failed.')
            diagnostic = report['diagnostics']
            print(f'{label}: Cut {report["end_to_end_seconds"] * 1000:.2f} ms, '
                  f'barrier {diagnostic["source"]["source_full_barrier"]["wall_ns"] / 1e6:.3f} ms', flush=True)
            if iteration >= 0:
                rows.append(dict(case=name, pump=mode, round=iteration + 1,
                                 cut_seconds=report['end_to_end_seconds'], diagnostics=diagnostic))
    groups = []
    for name, _, _, _, mode in cases:
        selected = [row for row in rows if row['case'] == name and row['pump'] == mode]
        domains = {}
        for domain in ('source', 'target', 'driver'):
            names = set(selected[0]['diagnostics'][domain])
            if any(set(row['diagnostics'][domain]) != names for row in selected):
                raise RuntimeError('Diagnostic stages differ between runs; inspect raw output.')
            domains[domain] = {stage: {metric.removesuffix('_ns') + '_ms': stats([
                row['diagnostics'][domain][stage][metric] / 1e6 for row in selected])
                for metric in ('wall_ns', 'cpu_ns', 'process_cpu_ns')} for stage in sorted(names)}
        groups.append(dict(case=name, pump=mode, samples=len(selected),
            cut_ms=stats([row['cut_seconds'] * 1000 for row in selected]), spans=domains,
            pump_turns=stats([row['diagnostics']['pump']['turns'] for row in selected]),
            empty_pump_turns=stats([row['diagnostics']['pump']['empty_turns'] for row in selected])))
    for name, digest in binaries.items():
        if hashlib.sha256((build / name).read_bytes()).hexdigest() != digest:
            raise RuntimeError('Binaries changed during profiling.')
    report = dict(passed=True, captured_utc=datetime.now(timezone.utc).isoformat(), environment=environment,
        binary_sha256=binaries, native_bytes=args.bytes, measured_runs=len(rows), discarded_runs=len(cases),
        summary=groups, samples=rows,
        scope='Instrumented loopback phase measurements, not perf sampling. Synthetic holds use worker sleep, not busy computation. Actual admitted occupancy is checked at Freeze; read-only prefetch and final data/history checks remain enabled.',
        accounting='Host wall clocks are local to each process. cpu is measuring-thread CPU; process_cpu includes all threads. Hash is nested in encoding/append, and transfer RPC includes receive/restore. Spans overlap and cannot be added indiscriminately. Per-chunk timestamp overhead is included.',
        polling='The poll/blocked comparison changes only the demo driver. Hosts continue using blocking GMainLoop and event-driven completion in both cases.')
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    for row in groups:
        src, dst, drv = (row['spans'][d] for d in ('source', 'target', 'driver'))
        print(f'{row["case"]:12} {row["pump"]:8} Cut={row["cut_ms"]["median"]:.2f} ms '
              f'Drain={src["source_business_drain"]["wall_ms"]["median"]:.3f} ms '
              f'Receive={dst["target_receive_and_hash"]["wall_ms"]["median"]:.2f} ms '
              f'DriverCPU={drv["driver_cut"]["cpu_ms"]["median"]:.3f} ms')


if __name__ == '__main__':
    main()
