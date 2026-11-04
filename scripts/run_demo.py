#!/usr/bin/env python3
"""Run the real four-process trusted-loopback Capstan demonstration.

Only Python's standard library is required. Credentials are temporary private
files and never written to logs or reports. Process logs and observed results
are retained independently from the abstract model and single-process tests.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import tempfile
import threading
import time
from typing import Callable


class ManagedProcess:
    def __init__(self, name: str, command: list[str], output: Path):
        self.name, self.command = name, command
        self.lines: list[str] = []
        self.condition = threading.Condition()
        self.log = (output / (name + '.log')).open('w', encoding='utf-8')
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.expected_exit: str | None = None

    def _read(self):
        assert self.process.stdout is not None
        for line in self.process.stdout:
            line = line.rstrip('\n')
            with self.condition:
                self.lines.append(line)
                self.log.write(line + '\n')
                self.log.flush()
                self.condition.notify_all()
        with self.condition:
            self.condition.notify_all()

    def mark(self) -> int:
        with self.condition:
            return len(self.lines)

    def wait_line(self, match: Callable[[str], bool], start=0, timeout=15.0) -> tuple[int, str]:
        deadline = time.monotonic() + timeout
        with self.condition:
            while True:
                for i in range(start, len(self.lines)):
                    if match(self.lines[i]):
                        return i, self.lines[i]
                remaining = deadline - time.monotonic()
                if remaining <= 0 or self.process.poll() is not None:
                    raise RuntimeError(f'{self.name}: no matching output; exit={self.process.poll()}, tail={self.lines[-8:]}')
                self.condition.wait(min(remaining, 0.2))

    def send(self, text: str) -> int:
        start = self.mark()
        if self.process.poll() is not None:
            raise RuntimeError(f'{self.name} already exited: {self.process.returncode}')
        assert self.process.stdin is not None
        self.process.stdin.write(text + '\n')
        self.process.stdin.flush()
        return start

    def wait_result(self, command: str, start=0, timeout=15.0) -> dict:
        label = command.split()[0]
        _, line = self.wait_line(lambda s: s.startswith('RESULT command=' + label + ' '), start, timeout)
        fields = dict(item.split('=', 1) for item in line.split()[1:])
        for key in list(fields):
            if key in ('outcome', 'ns') or re.fullmatch(r'v\d+', key):
                fields[key] = int(fields[key])
        return fields

    def command_result(self, command: str, expect='ok', timeout=15.0) -> dict:
        result = self.wait_result(command, self.send(command), timeout)
        if expect is not None and result['status'] != expect:
            raise AssertionError(f'{self.name} {command}: expected {expect}, got {result}')
        return result

    def stop(self):
        if self.process.poll() is None:
            if self.name.startswith('client'):
                try:
                    self.send('quit')
                    self.process.wait(timeout=2)
                    self.expected_exit = 'normal client quit'
                except (BrokenPipeError, RuntimeError, subprocess.TimeoutExpired):
                    pass
            if self.process.poll() is None:
                self.process.terminate()
                self.expected_exit = 'supervisor SIGTERM cleanup'
                try:
                    self.process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.expected_exit = 'supervisor SIGKILL cleanup'
                    self.process.wait(timeout=2)
        self.reader.join(timeout=2)
        self.log.close()
        if self.process.stdin:
            self.process.stdin.close()
        if self.process.stdout:
            self.process.stdout.close()


class DemoCluster:
    def __init__(self, build_dir: Path, output: Path, resource_bytes=64*1024*1024, work_delay_ms=100, host_binary="capstan_host", host_extra_args=None):
        self.build = build_dir.resolve()
        self.output = output.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.resource_bytes, self.work_delay_ms = resource_bytes, work_delay_ms
        self.host_binary, self.host_extra_args = host_binary, list(host_extra_args or [])
        self.children: list[ManagedProcess] = []
        self.private = tempfile.TemporaryDirectory(prefix='.private-', dir=self.output)
        self.identities = [secrets.randbits(63) or 1 for _ in range(4)]
        credentials = [secrets.token_hex(16) for _ in range(3)]
        self.config = Path(self.private.name) / 'lab.conf'
        self.write_private(self.config, ' '.join(map(str, self.identities)) + '\n' + '\n'.join(credentials) + '\n')
        self.role_paths = []
        for name, secret in zip(('A', 'B'), credentials):
            path = Path(self.private.name) / (name + '.credential')
            self.write_private(path, secret + '\n')
            self.role_paths.append(path)
        self.host: ManagedProcess
        self.table: ManagedProcess
        self.a: ManagedProcess
        self.b: ManagedProcess

    @staticmethod
    def write_private(path: Path, content: str):
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as stream:
            stream.write(content)

    def spawn(self, name: str, binary: str, *args: object) -> ManagedProcess:
        process = ManagedProcess(name, [str(self.build / binary), *map(str, args)], self.output)
        self.children.append(process)
        return process

    def __enter__(self):
        try:
            self.host = self.spawn('host', self.host_binary, '--config', self.config, '--port', 0,
                                   '--bytes', self.resource_bytes, '--work-delay-ms', self.work_delay_ms, *self.host_extra_args)
            _, line = self.host.wait_line(lambda s: s.startswith('READY host '))
            host_port = self.host_port = int(re.search(r'port=(\d+)', line).group(1))
            self.table = self.spawn('table', 'capstan_table', '--config', self.config,
                                    '--host-port', host_port, '--port', 0)
            _, line = self.table.wait_line(lambda s: s.startswith('READY table '))
            table_port = self.table_port = int(re.search(r'port=(\d+)', line).group(1))
            self.a = self.spawn('client_A', 'capstan_client', '--credential', self.role_paths[0],
                                '--host-port', host_port, '--table-port', table_port)
            self.b = self.spawn('client_B', 'capstan_client', '--credential', self.role_paths[1],
                                '--host-port', host_port, '--table-port', table_port)
            self.a.wait_line(lambda s: s.startswith('READY client '))
            self.b.wait_line(lambda s: s.startswith('READY client '))
            return self
        except BaseException:
            self.close()
            raise

    def close(self):
        for process in reversed(self.children):
            process.stop()
        self.private.cleanup()
        report = {'processes': [dict(name=p.name, command=p.command, exit_code=p.process.returncode,
                                    expected_exit=p.expected_exit) for p in self.children],
                  'credentials': 'temporary 0600 files deleted; values never logged',
                  'cluster_session': self.identities[0], 'host_incarnation': self.identities[1],
                  'object': self.identities[2], 'control_session': self.identities[3]}
        (self.output / 'processes.json').write_text(json.dumps(report, indent=2) + '\n')

    def __exit__(self, kind, error, traceback):
        self.close()


def values(result: dict) -> list[int]:
    return [result['v' + str(i)] for i in range(len([k for k in result if re.fullmatch(r'v\d+', k)]))]


def run_demo(build_dir: Path, output: Path, resource_bytes=64*1024*1024, work_delay_ms=100) -> dict:
    events = []
    checks = []
    def call(client, command, expect='ok'):
        result = client.command_result(command, expect)
        events.append({'client': client.name, 'input': command, **result})
        return result
    with DemoCluster(build_dir, output, resource_bytes, work_delay_ms) as lab:
        a, b = lab.a, lab.b
        call(a, 'shared')
        call(a, 'fetch'); call(b, 'fetch')
        before_a = call(a, 'summary'); before_b = call(b, 'summary')
        assert values(before_a) == values(before_b)
        assert before_a['v0'] * 8 == resource_bytes
        checks.append('two shared grants return the same Host-local Dataset summary')
        call(a, 'raw_write', 'forbidden')
        call(b, 'forged_mode', 'bad_grant')
        checks.append('shared writes and forged exclusive mode rejected by Host')
        table_before = call(a, 'table_stats')
        for _ in range(3):
            call(a, 'version'); call(b, 'version')
        table_after = call(a, 'table_stats')
        assert values(table_before) == values(table_after) and table_after['v2'] == 0
        checks.append('six warm business calls leave all directory counters unchanged')
        call(b, 'keep')
        call(a, 'exclusive 1'); call(a, 'fetch')
        call(b, 'stale', 'stale')
        call(a, 'add 0 1024 3')
        changed = call(a, 'summary')
        assert changed['v0'] == before_a['v0']
        assert changed['v1'] == (before_a['v1'] + 3072) % (1 << 64)
        assert changed['v3'] == before_a['v3'] + 1
        checks.append('exclusive update changes Host sum by exactly 3072 and version by one')
        call(a, 'keep')
        call(a, 'exclusive 2'); call(b, 'fetch')
        call(a, 'stale', 'stale')
        after_transfer = call(b, 'summary')
        assert values(after_transfer) == values(changed)
        checks.append('exclusive handoff fences the old holder and preserves resource contents')
        call(b, 'close')
        call(a, 'exclusive 2'); call(b, 'fetch')
        checks.append('explicit close receives acknowledgement before a replacement grant is fetched')
        # Establish that the summary is actually admitted before destroy starts.
        host_start = lab.host.mark()
        b_start = b.send('summary')
        lab.host.wait_line(lambda s: s.startswith('ADMIT '), host_start)
        destroy_start = a.send('destroy')
        transition_index, transition_line = lab.host.wait_line(
            lambda s: s.startswith('TRANSITION ') and 'status=pending' in s, host_start)
        summary_result = b.wait_result('summary', b_start)
        destroy_result = a.wait_result('destroy', destroy_start)
        assert summary_result['status'] == destroy_result['status'] == 'ok'
        events.append({'client': b.name, 'input': 'summary (in-flight at destroy)', **summary_result})
        events.append({'client': a.name, 'input': 'destroy', **destroy_result})
        complete_index, _ = lab.host.wait_line(lambda s: s.startswith('COMPLETE ') and 'active=0' in s, transition_index)
        destroy_index, _ = lab.host.wait_line(lambda s: s == 'DESTROY active=0', transition_index)
        assert transition_index < complete_index < destroy_index
        checks.append('destroy remains pending until admitted operation completes; destructor follows COMPLETE active=0')
        call(b, 'version', 'dead')
        stats = call(a, 'stats')
        assert stats['v0'] == stats['v1'] and stats['v4'] == 0 and stats['v6'] == 1
        wire_a, wire_b = call(a, 'wire'), call(b, 'wire')
        client_wire = sum(r['v0'] + r['v1'] for r in (wire_a, wire_b))
        assert client_wire < resource_bytes
        checks.append('actual client wire bytes are smaller than resident resource; no Dataset body returned')
        report = {'passed': True, 'evidence': 'real TCP, four independent localhost processes',
                  'cross_machine': 'NOT RUN', 'trusted_lab': True, 'resource_bytes': resource_bytes,
                  'client_wire_bytes': client_wire, 'warm_directory_delta': [0, 0, 0, 0],
                  'host_final': stats, 'checks': checks, 'events': events,
                  'limitations': ['Client wire count excludes table-to-host control link.',
                                  'Configured work delay is fault orchestration, not a performance measurement.',
                                  'No HA, recovery, authentication encryption, or hostile-network claim.']}
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=Path('build'))
    parser.add_argument('--output', type=Path)
    parser.add_argument('--bytes', type=int, default=64*1024*1024)
    parser.add_argument('--work-delay-ms', type=int, default=100)
    args = parser.parse_args()
    output = args.output or args.build_dir / 'demo'
    output.mkdir(parents=True, exist_ok=True)
    try:
        report = run_demo(args.build_dir, output, args.bytes, args.work_delay_ms)
        code = 0
    except Exception as error:
        report = {'passed': False, 'error': repr(error)}
        code = 1
    report['command_exit_code'] = code
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'events'}, indent=2))
    return code


if __name__ == '__main__':
    raise SystemExit(main())
