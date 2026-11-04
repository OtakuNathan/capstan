#!/usr/bin/env python3
"""Narrated A -> B -> A exclusive handoff, with Table stopped after bootstrap."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import signal
import time
from run_demo import DemoCluster, values


def require(condition: bool, message: str):
    if not condition:
        raise AssertionError(message)


def pause_table(process):
    """Wait for the actual child stop before demonstrating Table independence."""
    process.send_signal(signal.SIGSTOP)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        pid, state = os.waitpid(process.pid, os.WUNTRACED | os.WNOHANG)
        if pid:
            require(os.WIFSTOPPED(state), 'Table exited before its stop was confirmed')
            return
        time.sleep(.01)
    raise RuntimeError('Table did not stop within five seconds')


def run(build: Path, output: Path, resource_bytes: int, delay_ms: int) -> dict:
    events, steps = [], []

    def say(message):
        steps.append(message)
        print(message, flush=True)

    def call(client, command, expected='ok'):
        result = client.command_result(command, expected)
        events.append(dict(client=client.name, input=command, **result))
        return result

    def ref(receipt):
        return ' '.join(str(receipt[f'v{i}']) for i in range(7))

    def check_update(before, after, delta):
        require(after['v0'] == before['v0'], 'Dataset size changed')
        require(after['v1'] == (before['v1'] + delta) % (1 << 64), 'Unexpected Dataset sum')
        require(after['v3'] == before['v3'] + 1, 'Unexpected Dataset version')

    say('Direct exclusive transfer: A -> Host -> B -> Host -> A')
    say(f'Four real TCP processes; {resource_bytes // 1024} KiB stays on Host.')
    say(f'Business work delay: {delay_ms} ms to expose Drain. This is a demo, not a benchmark.')
    with DemoCluster(build, output, resource_bytes, delay_ms) as lab:
        a, b = lab.a, lab.b
        say('[1/7] Table bootstraps exclusive authority for A.')
        call(a, 'exclusive 1')
        initial_grant = call(a, 'fetch')
        initial = call(a, 'summary')
        require(initial['v0'] * 8 == resource_bytes, 'Unexpected resource size')
        table_before = call(a, 'table_stats')
        try:
            pause_table(lab.table.process)
            say('[2/7] Table is stopped. A adds 3 to elements [0, 1024).')
            call(a, 'add 0 1024 3')
            after_a = call(a, 'summary')
            check_update(initial, after_a, 3072)
            call(a, 'keep')
            say(f'      sum delta=3072; version={after_a["v3"]}; epoch={initial_grant["v0"]}.')

            say('[3/7] A starts summary, then transfers while that RPC is in flight.')
            mark = lab.host.mark()
            summary_start = a.send('summary')
            lab.host.wait_line(lambda line: line.startswith('ADMIT '), mark)
            transfer_start = a.send('transfer 2 1')
            pending_index, _ = lab.host.wait_line(
                lambda line: line.startswith('TRANSFER ') and 'status=pending' in line and 'active=1' in line, mark)
            early_ref = ' '.join(map(str, lab.identities[:3] + [1, 2, initial_grant['v0'], 1]))
            call(b, 'claim ' + early_ref, 'pending')
            say('      Host closed admission; B claim=Pending while the old pin drains.')
            installed = a.wait_result('transfer', transfer_start)
            completed = a.wait_result('summary', summary_start)
            events.extend([dict(client=a.name, input='transfer 2 1 (in-flight)', **installed),
                           dict(client=a.name, input='summary (in-flight)', **completed)])
            require(installed['status'] == completed['status'] == 'ok', 'In-flight transfer failed')
            require(values(completed) == values(after_a), 'Old RPC lost its original resource')
            require(installed['v7'] == initial_grant['v0'] + 1, 'Transfer did not increment epoch once')
            complete_index, _ = lab.host.wait_line(
                lambda line: line.startswith('COMPLETE ') and 'active=0' in line, pending_index)
            require(complete_index > pending_index, 'Installation did not wait for Drain')

            say('[4/7] B authenticates and claims its new grant directly from Host.')
            claimed = call(b, 'claim ' + ref(installed))
            require(claimed['v0'] == installed['v7'], 'B received an unexpected epoch')
            require(values(call(b, 'summary')) == values(after_a), 'Handoff changed the data')
            call(a, 'version', 'invalid')
            call(a, 'stale', 'stale')
            say(f'      Installed epoch={claimed["v0"]}; A wrapper is sealed; A old grant is Stale.')

            say('[5/7] B adds 5 to elements [1024, 2048), then transfers back to A.')
            call(b, 'add 1024 2048 5')
            after_b = call(b, 'summary')
            check_update(after_a, after_b, 5120)
            call(b, 'keep')
            back = call(b, 'transfer 1 1')
            require(back['v7'] == installed['v7'] + 1, 'Return transfer did not increment epoch once')
            received = call(a, 'claim ' + ref(back))
            require(received['v0'] == back['v7'], 'A received an unexpected return epoch')
            final = call(a, 'summary')
            require(values(final) == values(after_b), 'Return handoff changed the data')
            call(b, 'stale', 'stale')
            say(f'      A owns epoch={received["v0"]}; total sum delta=8192; version={final["v3"]}.')
        finally:
            # Resume before DemoCluster cleanup, including on assertion/Ctrl-C.
            if lab.table.process.poll() is None:
                lab.table.process.send_signal(signal.SIGCONT)

        say('[6/7] Resume Table and check that direct work made zero directory calls.')
        table_after = call(a, 'table_stats')
        directory_delta = [after - before for before, after in zip(values(table_before), values(table_after))]
        require(directory_delta == [0, 0, 0, 0], 'Direct path unexpectedly contacted Table')
        call(b, 'fetch', 'bad_grant')
        say('      Directory delta=[0, 0, 0, 0]; resumed Table refuses the old holder B.')
        host = call(a, 'stats')
        require(host['v0'] == host['v1'] and host['v4'] == 0 and host['v6'] == 0,
                'Host has unfinished work or destroyed the resource')
        say('[7/7] Table management still works: destroy after all pins have drained.')
        call(a, 'destroy')
        destroyed = call(a, 'stats')
        require(destroyed['v4'] == 0 and destroyed['v6'] == 1, 'Resource did not destroy exactly once after Drain')
        say('PASS: A -> B -> A preserved the Dataset; both authority routes remain usable.')
        report = dict(passed=True, evidence='four independent processes, real loopback TCP',
                      resource_bytes=resource_bytes, work_delay_ms=delay_ms, benchmark=False,
                      epochs=[initial_grant['v0'], installed['v7'], back['v7']],
                      total_sum_delta=8192, version_delta=final['v3'] - initial['v3'],
                      directory_direct_path_delta=directory_delta,
                      host_before_destroy=host, host_after_destroy=destroyed,
                      steps=steps, events=events)
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=Path('build'))
    parser.add_argument('--output', type=Path, help='Default: BUILD_DIR/direct-demo')
    parser.add_argument('--bytes', type=int, default=64 * 1024 * 1024, help='Host Dataset size, at least 16384 bytes')
    parser.add_argument('--work-delay-ms', type=int, default=250, help='100..1000; exposes the in-flight drain, not a performance setting')
    args = parser.parse_args()
    if not 16384 <= args.bytes <= 256 * 1024 * 1024 or args.bytes % 8:
        parser.error('--bytes must be a multiple of 8 between 16384 and 268435456')
    if not 100 <= args.work_delay_ms <= 1000:
        parser.error('--work-delay-ms must be between 100 and 1000')
    output = args.output or args.build_dir / 'direct-demo'
    output.mkdir(parents=True, exist_ok=True)
    try:
        report = run(args.build_dir, output, args.bytes, args.work_delay_ms)
        code = 0
    except KeyboardInterrupt:
        report, code = dict(passed=False, error='Interrupted'), 130
    except Exception as error:
        report, code = dict(passed=False, error=repr(error)), 1
    report['command_exit_code'] = code
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if code:
        print('FAIL: ' + report['error'], flush=True)
    print(f'Report and process logs: {output.resolve()}', flush=True)
    return code


if __name__ == '__main__':
    raise SystemExit(main())
