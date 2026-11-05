#!/usr/bin/env python3
"""Native closed-loop concurrency batches with explicit bounded history."""
import argparse
import collections
import csv
import hashlib
import json
import math
from pathlib import Path
import platform
import random
import statistics
from run_demo import DemoCluster, values

ROOT = Path(__file__).resolve().parents[1]


def quantiles(samples):
    ordered = sorted(samples)
    return {'n': len(ordered), **{
        f'p{q}_us': ordered[math.ceil(len(ordered) * q / 100) - 1] / 1000
        for q in (50, 95, 99)}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--overwrite', action='store_true', help='replace an earlier report in this output directory')
    parser.add_argument('--workloads', default='version')
    parser.add_argument('--concurrency', default='1,2,4,8,16,32,48')
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--samples', type=int, default=3000)
    parser.add_argument('--summary-samples', type=int, default=256)
    parser.add_argument('--warmup', type=int, default=128)
    parser.add_argument('--history-limit', type=int, default=4096)
    parser.add_argument('--inline-version', type=int, choices=(0, 1), default=1)
    parser.add_argument('--trace-requests', type=int, choices=(0, 1), default=0)
    parser.add_argument('--resource-bytes', type=int, default=64 * 1024 * 1024)
    args = parser.parse_args()
    workloads = args.workloads.split(',')
    try:
        levels = [int(n) for n in args.concurrency.split(',')]
    except ValueError:
        parser.error('concurrency must be comma-separated integers')
    if (not levels or len(set(levels)) != len(levels) or
            any(n < 1 or n > 56 for n in levels) or
            not workloads or any(w not in ('version', 'summary') for w in workloads)):
        parser.error('use distinct concurrency levels 1..56 and version/summary workloads')
    if not 1 <= args.rounds <= 20 or not 0 <= args.warmup <= 65536:
        parser.error('rounds 1..20, warmup 0..65536')
    if not 1 <= args.history_limit <= 1048576:
        parser.error('history-limit 1..1048576')
    if not 8 <= args.resource_bytes <= 256 * 1024 * 1024 or args.resource_bytes % 8:
        parser.error('resource-bytes must be 8-byte aligned, 8 bytes..256 MiB')
    for workload in workloads:
        count = args.samples if workload == 'version' else args.summary_samples
        if count <= 0 or count + args.warmup + 2 > args.history_limit:
            parser.error('samples + warmup + two validation calls must fit history-limit')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if (output / 'report.json').exists() and not args.overwrite:
        parser.error('output already contains a report; choose a fresh directory')
    build = args.build_dir.resolve()
    options = vars(args).copy()
    options.update(build_dir=str(build), output=str(output))
    report = {'passed': False, 'options': options, 'platform': platform.platform(),
              'binary_sha256': {role: hashlib.sha256((build / ('capstan_' + role)).read_bytes()).hexdigest()
                                for role in ('host', 'table', 'client', 'throughput')},
              'source_sha256': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
                                for name in ('src/apps/host.cpp', 'src/apps/throughput.cpp',
                                             'src/apps/common.hpp', 'src/apps/authority_replies.hpp',
                                             'include/capstan/transport/context_dispatch.hpp',
                                             'src/transport/gio.cpp', 'include/capstan/authority/host.hpp',
                                             'scripts/bench_concurrency.py', 'scripts/run_demo.py', 'deps.lock.json')},
              'runs': [],
              'scope': 'One shared object, loopback TCP, native closed-loop batches; one outstanding RPC per connection.',
              'limitations': [
                  'Bounded history is not retired: longer batches require an explicit larger finite capacity.',
                  'No open-loop arrival-rate, cross-machine, multiple-object, or indefinite-service claim.',
                  'One load-generator event loop; throughput includes its scheduling and response validation.',
                  'Wall time includes batch ramp-up/drain, excludes setup, warmup and sample-file output.',
                  'Host logging and inline-scalar policy are explicit options; comparisons must identify both.']}
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    grouped = collections.defaultdict(list)
    try:
        for workload in workloads:
            count = args.samples if workload == 'version' else args.summary_samples
            for repeat in range(args.rounds):
                order = list(levels)
                if repeat == 1:
                    order.reverse()
                elif repeat > 1:
                    random.Random(1307 + repeat).shuffle(order)
                for concurrency in order:
                    name = f'{workload}-r{repeat}-c{concurrency}'
                    folder = output / name
                    with DemoCluster(build, folder, args.resource_bytes, 0,
                                     host_extra_args=['--trace-requests', args.trace_requests,
                                                      '--inline-version', args.inline_version,
                                                      '--history-limit', args.history_limit]) as lab:
                        lab.a.command_result('shared')
                        lab.a.command_result('fetch')
                        expected = values(lab.a.command_result('summary'))
                        before = values(lab.a.command_result('stats'))
                        driver = lab.spawn('load', 'capstan_throughput',
                                           '--credential', lab.role_paths[1],
                                           '--host-port', lab.host_port, '--table-port', lab.table_port,
                                           '--workload', workload, '--concurrency', concurrency,
                                           '--count', count, '--warmup', args.warmup,
                                           '--expected-count', expected[0], '--expected-sum', expected[1],
                                           '--expected-checksum', expected[2], '--output', folder / 'samples.csv')
                        driver.process.wait(timeout=60)
                        driver.reader.join(timeout=2)
                        assert driver.process.returncode == 0, driver.lines[-8:]
                        line = next(line for line in driver.lines if line.startswith('LOAD '))
                        counters = {k: int(v) for k, v in (item.split('=') for item in line.split()[1:])}
                        with (folder / 'samples.csv').open() as stream:
                            rows = list(csv.DictReader(stream))
                        valid = [int(row['ns']) for row in rows if row['valid'] == '1']
                        assert len(rows) == len(valid) == counters['ok'] == count
                        assert counters['errors'] == counters['warm_errors'] == 0
                        assert len({row['sequence'] for row in rows}) == count
                        assert values(lab.a.command_result('summary')) == expected
                        after = values(lab.a.command_result('stats'))
                        assert after[0] == after[1] == count + args.warmup + 2 and after[4] == 0
                        record = {'name': name, 'workload': workload, 'concurrency': concurrency,
                                  'repeat': repeat, 'counters': counters, 'latency': quantiles(valid),
                                  'success_per_second': count * 1e9 / counters['wall_ns'],
                                  'host_before': before, 'host_after': after,
                                  'status_counts': dict(collections.Counter(row['status'] for row in rows))}
                    report['runs'].append(record)
                    grouped[f'{workload}/{concurrency}'].extend(valid)
                    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
                    print(name, round(record['success_per_second']), 'RPC/s', record['latency'], flush=True)
        report['summary'] = {}
        for key, samples in grouped.items():
            workload, concurrency = key.split('/')
            rates = [r['success_per_second'] for r in report['runs']
                     if r['workload'] == workload and r['concurrency'] == int(concurrency)]
            report['summary'][key] = {'latency': quantiles(samples),
                                      'throughput_median': statistics.median(rates),
                                      'throughput_min': min(rates), 'throughput_max': max(rates)}
        report['passed'] = True
    except BaseException as error:
        report['error'] = repr(error)
        raise
    finally:
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
