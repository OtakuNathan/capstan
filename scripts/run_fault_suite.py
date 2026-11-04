#!/usr/bin/env python3
"""Deterministic real-process disconnect/drain scenario plus socket fault tests."""
from __future__ import annotations
import argparse
import json
import re
import secrets
from pathlib import Path
import subprocess
from run_demo import DemoCluster, values


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=Path('build'))
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    output = args.output or args.build_dir / 'fault-suite'
    output.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'checks': [], 'events': [], 'cross_machine': 'NOT RUN'}
    try:
        with (output / 'transport.log').open('w') as log:
            run = subprocess.run([str((args.build_dir / 'transport_test').resolve())], stdout=log, stderr=subprocess.STDOUT, timeout=30)
        report['transport_exit_code'] = run.returncode
        assert run.returncode == 0, 'socket fault test failed'
        report['checks'].append('real socket fragmentation/coalescing/disconnect/frame/backpressure/session bounds')
        with DemoCluster(args.build_dir, output / 'disconnect', 1024*1024, 300) as lab:
            a, b = lab.a, lab.b
            def call(client, command, expect='ok'):
                result = client.command_result(command, expect)
                report['events'].append({'client': client.name, 'input': command, **result})
                return result
            call(a, 'shared'); call(a, 'fetch'); call(b, 'fetch')
            initial = call(a, 'summary')
            call(a, 'exclusive 2'); call(b, 'fetch')
            host_start = lab.host.mark()
            b.send('add 0 1 10')
            admit_index, _ = lab.host.wait_line(lambda s: s.startswith('ADMIT '), host_start)
            b.process.kill()
            b.process.wait(timeout=3)
            b.expected_exit = 'intentional SIGKILL after Host admission, before client result'
            in_flight = call(a, 'stats')
            assert in_flight['v4'] == 1, 'client death incorrectly released Host pin or observation missed delayed operation'
            transfer_start = a.send('exclusive 1')
            transition_index, _ = lab.host.wait_line(lambda s: s.startswith('TRANSITION ') and 'status=pending' in s, admit_index)
            transfer = a.wait_result('exclusive', transfer_start)
            assert transfer['status'] == 'ok'
            report['events'].append({'client': a.name, 'input': 'exclusive 1 after client death', **transfer})
            complete_index, _ = lab.host.wait_line(lambda s: s.startswith('COMPLETE ') and 'active=0' in s, transition_index)
            assert admit_index < transition_index < complete_index
            call(a, 'fetch')
            observed = call(a, 'summary')
            assert observed['v1'] == (initial['v1'] + 10) % (1 << 64)
            assert observed['v3'] == initial['v3'] + 1
            stats = call(a, 'stats')
            assert stats['v4'] == 0 and stats['v0'] == stats['v1']
            # Initial summary, one abandoned write, and final summary. No retry.
            assert stats['v0'] == 3
            report['checks'] += [
                'client SIGKILL after admission leaves Host active pin intact',
                'next writer receives grant only after actual old operation completion',
                'abandoned write executes once; sum +10, version +1; no automatic retry',
                'three admitted operations exactly match initial read, abandoned write, final read']
            report['client_observation'] = 'OutcomeUnknown: killed before response; execution established only by later Host observation'
            report['host_final'] = stats
        # A live client times out locally while its accepted write continues.
        # Separate cluster keeps request/session identities and observations clear.
        with DemoCluster(args.build_dir, output / 'timeout', 1024*1024, 300) as lab:
            a, b = lab.a, lab.b
            def call_timeout(client, command, expect='ok'):
                result = client.command_result(command, expect)
                report['events'].append({'scenario': 'timeout', 'client': client.name, 'input': command, **result})
                return result
            call_timeout(a, 'shared'); call_timeout(a, 'fetch'); call_timeout(b, 'fetch')
            initial = call_timeout(a, 'summary')
            call_timeout(a, 'exclusive 2'); call_timeout(b, 'fetch')
            host_start = lab.host.mark()
            timed_out = call_timeout(b, 'timeout_add 0 1 7 10', expect='invalid')
            assert timed_out['outcome'] == 3, 'local deadline must report OutcomeUnknown'
            admit_index, _ = lab.host.wait_line(lambda s: s.startswith('ADMIT '), host_start)
            in_flight = call_timeout(a, 'stats')
            assert in_flight['v4'] == 1, 'local timeout released remote pin'
            transfer_start = a.send('exclusive 1')
            transition_index, _ = lab.host.wait_line(lambda s: s.startswith('TRANSITION ') and 'status=pending' in s, admit_index)
            transfer = a.wait_result('exclusive', transfer_start)
            assert transfer['status'] == 'ok'
            report['events'].append({'scenario': 'timeout', 'client': a.name, 'input': 'exclusive 1 after timeout', **transfer})
            complete_index, _ = lab.host.wait_line(lambda s: s.startswith('COMPLETE ') and 'active=0' in s, transition_index)
            assert admit_index < transition_index < complete_index
            call_timeout(a, 'fetch')
            observed = call_timeout(a, 'summary')
            assert observed['v1'] == (initial['v1'] + 7) % (1 << 64)
            assert observed['v3'] == initial['v3'] + 1
            call_timeout(b, 'wire')  # Live loop has consumed the late response.
            callbacks = [line for line in b.lines if line.startswith('RESULT command=timeout_add ')]
            assert len(callbacks) == 1, 'late reply completed timed-out awaitable again'
            stats = call_timeout(a, 'stats')
            assert stats['v0'] == stats['v1'] == 3 and stats['v4'] == 0
            report['timeout_host_final'] = stats
            report['checks'] += [
                'real TCP local deadline returns OutcomeUnknown while Host pin remains active',
                'replacement writer waits for timed-out operation to finish',
                'late reply does not callback destroyed wait frame; timeout completes once',
                'timed-out write executes once, sum +7/version +1, with no retry']
        with DemoCluster(args.build_dir, output / 'malformed', 1024*1024, 0) as lab:
            a = lab.a
            def malformed_call(command, expect='ok'):
                result = a.command_result(command, expect)
                report['events'].append({'scenario': 'malformed', 'client': a.name, 'input': command, **result})
                return result
            malformed_call('exclusive 1'); malformed_call('fetch')
            initial = malformed_call('summary')
            cases = [('bad_method', 'unknown_method'), ('bad_type', 'invalid'),
                     ('wrong_object', 'wrong_object'), ('wrong_token', 'bad_grant'),
                     ('bad_version', 'invalid')]
            categories = {}
            for command, expected in cases:
                result = malformed_call(command, expected)
                categories[command] = {'status': result['status'], 'outcome': result['outcome']}
            observed = malformed_call('summary')
            assert values(initial) == values(observed), 'malformed operation changed the resource'
            assert categories['bad_type']['outcome'] == 2
            assert all(categories[c]['outcome'] == 1 for c in ('bad_method', 'wrong_object', 'wrong_token', 'bad_version'))
            report['malformed_categories'] = categories
            report['checks'] += [
                'actual TCP unknown method, wrong argument type, wrong object/token and invalid codec version rejected',
                'malformed requests preserve sum/checksum/version; rejected versus completed-business-error categories recorded']
        # Installed grant with no client fetch: this is a post-install,
        # pre-delivery crash, not a claim that we dropped a network grant packet.
        with DemoCluster(args.build_dir, output / 'undelivered-and-new-directory', 1024*1024, 0) as lab:
            a, b = lab.a, lab.b
            def lifecycle_call(client, command, expect='ok'):
                result = client.command_result(command, expect)
                report['events'].append({'scenario': 'lifecycle', 'client': client.name, 'input': command, **result})
                return result
            lifecycle_call(a, 'shared'); lifecycle_call(a, 'fetch')
            initial = lifecycle_call(a, 'summary')
            installed = lifecycle_call(a, 'exclusive 2')
            installed_stats = lifecycle_call(a, 'stats')
            assert installed_stats['v5'] == installed['v0']
            assert not any(line.startswith('RESULT command=fetch ') for line in b.lines)
            b.process.kill(); b.process.wait(timeout=3)
            b.expected_exit = 'intentional SIGKILL after exclusive grant installation, before any grant fetch'
            lifecycle_call(a, 'version', 'stale')
            lifecycle_call(a, 'fetch', 'bad_grant')
            after_crash = lifecycle_call(a, 'stats')
            assert after_crash['v5'] == installed_stats['v5'] and after_crash['v4'] == 0
            explicit = lifecycle_call(a, 'exclusive 1')
            assert explicit['v0'] == installed['v0'] + 1
            lifecycle_call(a, 'fetch')
            observed = lifecycle_call(a, 'summary')
            assert values(initial) == values(observed)
            report['checks'] += [
                'client killed after exclusive grant installed but before first fetch; no network delivery-drop claim',
                'undelivered grant does not trigger automatic reassignment; old holder stays stale and fetch is refused',
                'explicit authority transition restores availability with a new epoch and unchanged resource']
            # Same object and lab credentials, but a new empty directory/control
            # session. It must not take over a live Host or publish fresh grants.
            fields = lab.config.read_text().split()
            replacement_control = secrets.randbits(63) or 1
            while replacement_control == int(fields[3]):
                replacement_control = secrets.randbits(63) or 1
            fields[3] = str(replacement_control)
            new_config = Path(lab.private.name) / 'new-directory.conf'
            lab.write_private(new_config, ' '.join(fields[:4]) + '\n' + '\n'.join(fields[4:]) + '\n')
            table2 = lab.spawn('table_new', 'capstan_table', '--config', new_config,
                               '--host-port', lab.host_port, '--port', 0)
            _, ready = table2.wait_line(lambda line: line.startswith('READY table '))
            new_port = int(re.search(r'port=(\d+)', ready).group(1))
            client2 = lab.spawn('client_A_new', 'capstan_client', '--credential', lab.role_paths[0],
                                '--host-port', lab.host_port, '--table-port', new_port)
            client2.wait_line(lambda line: line.startswith('READY client '))
            before_takeover = lifecycle_call(a, 'stats')
            refused = lifecycle_call(client2, 'shared', 'wrong_control')
            assert refused['outcome'] == 1
            lifecycle_call(client2, 'fetch', 'wrong_control')
            new_table_stats = lifecycle_call(client2, 'table_stats')
            assert new_table_stats['v3'] == 0
            after_takeover = lifecycle_call(a, 'stats')
            assert after_takeover['v5'] == before_takeover['v5']
            lifecycle_call(a, 'version')  # Original installed authority still usable.
            report['new_directory_result'] = {'status': refused['status'], 'outcome': refused['outcome'],
                                              'new_directory_epoch': new_table_stats['v3'],
                                              'host_epoch_before': before_takeover['v5'],
                                              'host_epoch_after': after_takeover['v5']}
            report['checks'] += [
                'new empty directory with a different control session cannot take over the live Host',
                'failed takeover publishes no grant and leaves original Host epoch/authority usable']
        # Bypass the client exclusive-operation gate on purpose: multiple TCP
        # connections authenticate as the same granted holder. Host admission
        # must still queue and serialize all accepted business writes.
        with DemoCluster(args.build_dir, output / 'same-holder-writes', 1024*1024, 20) as lab:
            a = lab.a
            def serial_call(command, expect='ok'):
                result = a.command_result(command, expect)
                report['events'].append({'scenario': 'same_holder_writes', 'client': a.name, 'input': command, **result})
                return result
            serial_call('exclusive 1'); serial_call('fetch')
            initial = serial_call('summary')
            host_start = lab.host.mark()
            load = lab.spawn('load_same_holder', 'capstan_load', '--credential', lab.role_paths[0],
                             '--host-port', lab.host_port, '--table-port', lab.table_port,
                             '--concurrency', 8, '--write', 1)
            _, line = load.wait_line(lambda line: line.startswith('LOAD '), timeout=20)
            metrics = {key: int(value) for key, value in (field.split('=', 1) for field in line.split()[1:])}
            assert load.process.wait(timeout=3) == 0
            load.expected_exit = 'completed same-holder concurrent write burst'
            assert metrics['concurrency'] == 8 and metrics['attempts'] == 16
            assert metrics['ok'] == 8 and metrics['not_submitted'] == 8
            assert metrics['unknown'] == metrics['rejected'] == 0
            observed = serial_call('summary')
            assert observed['v1'] == (initial['v1'] + 8) % (1 << 64)
            assert observed['v3'] == initial['v3'] + 8
            active_counts = [int(re.search(r'active=(\d+)', line).group(1))
                             for line in lab.host.lines[host_start:] if line.startswith('ADMIT ')]
            assert max(active_counts) > 1, 'did not observe multiple simultaneous admitted operations'
            stats = serial_call('stats')
            assert stats['v0'] == stats['v1'] == 10 and stats['v4'] == 0
            report['same_holder_writes'] = {**metrics, 'peak_admitted_operations': max(active_counts),
                                           'sum_increment': 8, 'version_increment': 8, 'host_final': stats}
            report['checks'] += [
                'eight same-exclusive-holder TCP connections admit overlapping writes while Host execution remains serialized',
                'eight writes produce exactly sum +8/version +8; all ten operations including two reads complete with zero active',
                'second immediate request on each connection returns NotSubmitted without remote side effects']
        report['passed'] = True
        code = 0
    except Exception as error:
        report['error'] = repr(error)
        code = 1
    report['command_exit_code'] = code
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'events'}, indent=2))
    return code


if __name__ == '__main__':
    raise SystemExit(main())
