#!/usr/bin/env python3
"""Measure sequential Copy/Paste and Cut/Paste on two fresh loopback Hosts per run."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import sys


def stats(values):
    return dict(median=statistics.median(values), min=min(values), max=max(values))


def summarize(samples, sizes):
    summary = []
    for size in sizes:
        rows = [sample for sample in samples if sample['native_bytes'] == size * 1048576]
        phases = {name.removesuffix('_seconds') + '_ms':
                  stats([row['phase_seconds'][name] * 1000 for row in rows])
                  for name in ('copy_seconds', 'first_paste_seconds', 'second_paste_seconds')}
        summary.append(dict(native_mib=size, samples=len(rows), **phases,
            cut_ms=stats([row['end_to_end_seconds'] * 1000 for row in rows]),
            close_to_activation_ms=stats([row['observed_close_to_activation_seconds'] * 1000 for row in rows]),
            cut_package_mib_per_second=stats([row['MiB_per_second'] for row in rows]),
            paste_native_mib_per_second={phase: stats([size / row['phase_seconds'][phase + '_seconds'] for row in rows])
                                         for phase in ('first_paste', 'second_paste')},
            host_peak_rss_mib=[stats([row['host_peak_rss_kib'][host] / 1024 for row in rows])
                               if all(row['host_peak_rss_kib'][host] is not None for row in rows) else None
                               for host in (0, 1)]))
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sizes-mib', type=int, nargs='+', default=[1, 8, 32, 64])
    parser.add_argument('--rounds', type=int, default=5)
    args = parser.parse_args()
    if args.rounds < 2 or any(size <= 0 or size > 64 for size in args.sizes_mib):
        parser.error('Use at least two rounds and sizes in 1..64 MiB (default demo quota).')
    build, output = args.build_dir.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binary_hashes = {name: hashlib.sha256((build / name).read_bytes()).hexdigest()
                     for name in ('capstan_host', 'capstan_migration_demo')}
    samples, environment = [], None
    # One discarded run per size; every run has fresh Host processes. Rotate size
    # order between rounds to reduce systematic time/order bias.
    for round_index in range(-1, args.rounds):
        offset = max(round_index, 0) % len(args.sizes_mib)
        sizes = args.sizes_mib[offset:] + args.sizes_mib[:offset]
        for size in sizes:
            label = 'warmup' if round_index < 0 else f'round-{round_index + 1}'
            destination = output / f'{label}-{size}m'
            result = subprocess.run([sys.executable, str(Path(__file__).with_name('run_migration_demo.py')),
                                     '--build-dir', str(build), '--output', str(destination),
                                     '--bytes', str(size * 1048576)], text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if result.returncode:
                raise RuntimeError(f'{label} {size} MiB failed:\n{result.stdout}')
            sample = json.loads((destination / 'report.json').read_text())
            if not sample['passed'] or len(sample['phase_seconds']) != 3:
                raise RuntimeError('Rebuild the demo with phase timing before benchmarking.')
            if environment is None:
                environment = sample['environment']
            if environment != sample['environment']:
                raise RuntimeError('Build/environment changed between runs.')
            print(f'{label} {size} MiB: Cut {sample["end_to_end_seconds"] * 1000:.2f} ms, '
                  f'{sample["MiB_per_second"]:.2f} MiB/s', flush=True)
            if round_index >= 0:
                sample.pop('environment')
                sample.pop('scope')
                samples.append(dict(round=round_index + 1, **sample))
    if any(hashlib.sha256((build / name).read_bytes()).hexdigest() != digest
           for name, digest in binary_hashes.items()):
        raise RuntimeError('Executables changed during the benchmark.')
    summary = summarize(samples, args.sizes_mib)
    report = dict(passed=True, captured_utc=datetime.now(timezone.utc).isoformat(),
                  environment=environment, binary_sha256=binary_hashes,
                  rounds=args.rounds, discarded_runs_per_size=1, summary=summary, samples=samples,
                  scope='Sequential loopback end-to-end operations, fresh processes each run; no saturation or p99 claim.',
                  timing='Copy captures a snapshot; each Paste transfers and installs one clone. Cut includes reservation, Drain, transfer, activation and cleanup. Query and validation RPCs are outside phase timers. Driver observation includes its 100 us polling sleep.',
                  memory='Host VmHWM covers the whole demo, including two retained clones; not incremental migration memory.',
                  close_interval='Coordinator Freeze send to activation ACK; not exact Host gate downtime.')
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('MiB | Copy ms | Paste #1 ms | Paste #2 ms | Cut ms | Cut MiB/s | Close→ACK ms')
    for row in summary:
        print(f'{row["native_mib"]:3} | ' + ' | '.join(f'{row[key]["median"]:.2f}' for key in
              ('copy_ms', 'first_paste_ms', 'second_paste_ms', 'cut_ms',
               'cut_package_mib_per_second', 'close_to_activation_ms')))


if __name__ == '__main__':
    main()
