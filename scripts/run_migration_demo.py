#!/usr/bin/env python3
"""Run two independent Hosts and the session-owned Table migration driver."""
import argparse
import json
from pathlib import Path
import re
import secrets
import subprocess
import threading
import queue
import platform
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=Path('build'))
    parser.add_argument('--output', type=Path, default=Path('build/migration-demo'))
    parser.add_argument('--bytes', type=int, default=1024 * 1024)
    parser.add_argument('--profile', action='store_true')
    parser.add_argument('--inflight', choices=['none', 'read', 'write'], default='none')
    parser.add_argument('--inflight-count', type=int, default=1)
    parser.add_argument('--work-delay-ms', type=int, default=0)
    parser.add_argument('--pump-mode', choices=['poll', 'blocking'], default='poll')
    args = parser.parse_args()
    if not 0 <= args.work_delay_ms <= 1000 or not 1 <= args.inflight_count <= 8:
        parser.error('work-delay-ms must be 0..1000 and inflight-count must be 1..8')
    build, output = args.build_dir.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    config = output / 'session.conf'
    config.write_text('1 2 3 7\n' + '\n'.join(secrets.token_hex(16) for _ in range(3)) + '\n')
    config.chmod(0o600)
    hosts, logs, readers = [], [], []
    try:
        ports = []
        for host_id in (1, 2):
            process = subprocess.Popen([str(build / 'capstan_host'), '--config', str(config),
                '--port', '0', '--host-id', str(host_id), '--empty', str(host_id == 2 and 1 or 0),
                '--bytes', str(args.bytes), '--trace-requests', '0',
                '--migration-metrics', str(int(args.profile)),
                '--work-delay-ms', str(args.work_delay_ms if host_id == 1 else 0)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            hosts.append(process)
            ready = queue.Queue()
            log = (output / f'host-{host_id}.log').open('w')
            logs.append(log)
            def drain(proc=process, stream=log, notifications=ready):
                for line in proc.stdout:
                    stream.write(line)
                    stream.flush()
                    if line.startswith('READY '):
                        notifications.put(line)
                notifications.put(None)
            reader = threading.Thread(target=drain, daemon=True)
            readers.append(reader)
            reader.start()
            line = ready.get(timeout=20)
            if not line:
                raise RuntimeError('Host failed before readiness')
            ports.append(re.search(r'port=(\d+)', line).group(1))
        result = subprocess.run([str(build / 'capstan_migration_demo'), '--config', str(config),
            '--source-port', ports[0], '--target-port', ports[1], '--profile', str(int(args.profile)),
            '--inflight', args.inflight, '--inflight-count', str(args.inflight_count),
            '--pump-mode', args.pump_mode], text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
        (output / 'demo.log').write_text(result.stdout)
        print(result.stdout, end='')
        if result.returncode or 'PASS host_to_host_migration' not in result.stdout:
            raise RuntimeError('Migration demo failed')
        measurement = re.search(r'MEASURE package_bytes=(\d+) seconds=([\d.e+-]+) MiB_per_second=([\d.e+-]+)', result.stdout)
        peaks = []
        for host in hosts:
            status_path = Path(f'/proc/{host.pid}/status')
            match = re.search(r'^VmHWM:\s+(\d+) kB', status_path.read_text(), re.M) if status_path.exists() else None
            peaks.append(int(match[1]) if match else None)
        downtime = re.search(r'close_to_activation_seconds=([\d.e+-]+)', result.stdout)
        phases = re.search(r'^PHASE (.+)$', result.stdout, re.M)
        phase_seconds = {name: float(value) for name, value in
                         (field.split('=') for field in phases[1].split())} if phases else {}
        diagnostics = None
        if args.profile:
            def metrics(text):
                return {fields['name']: {k: int(v) for k, v in fields.items() if k != 'name'}
                        for fields in (dict(field.split('=', 1) for field in line.split()[1:])
                                       for line in text.splitlines() if line.startswith('PROFILE '))}
            deadline = time.monotonic() + 2
            while True:
                source_log = (output / 'host-1.log').read_text()
                target_log = (output / 'host-2.log').read_text()
                source_metrics, target_metrics = metrics(source_log), metrics(target_log)
                if {'source_hash', 'source_full_barrier', 'source_business_drain'} <= source_metrics.keys() and {'target_receive_and_hash', 'target_restore'} <= target_metrics.keys():
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError('Missing Host diagnostic spans')
                time.sleep(.01)
            occupancy = re.search(r'^OCCUPANCY readers=(\d+) writers=(\d+) captures=(\d+)$', source_log, re.M)
            pump = re.search(r'^PUMP turns=(\d+) empty_turns=(\d+)$', result.stdout, re.M)
            if not occupancy or not pump:
                raise RuntimeError('Missing occupancy/pump evidence')
            counts = dict(zip(('readers', 'writers', 'captures'), map(int, occupancy.groups())))
            expected = args.inflight_count if args.inflight != 'none' else 0
            if counts['readers'] != (expected if args.inflight == 'read' else 0) or counts['writers'] != (expected if args.inflight == 'write' else 0):
                raise RuntimeError('Requested in-flight occupancy was not present at Freeze')
            diagnostics = dict(source=source_metrics, target=target_metrics, driver=metrics(result.stdout),
                occupancy_at_freeze=counts, pump=dict(turns=int(pump[1]), empty_turns=int(pump[2])),
                workload=dict(inflight=args.inflight, count=expected, injected_delay_ms=args.work_delay_ms, pump=args.pump_mode),
                scope='Host spans use same-process clocks. cpu_ns is measuring-thread CPU; process_cpu_ns includes all threads. Nested/overlapping spans must not be summed. Injected hold is sleep, not busy computation.')
        cache = {}
        for line in (build / 'CMakeCache.txt').read_text().splitlines():
            if ':' in line and '=' in line and not line.startswith(('//', '#')):
                name, value = line.split('=', 1)
                cache[name.split(':', 1)[0]] = value
        compiler = subprocess.run([cache['CMAKE_CXX_COMPILER'], '--version'], text=True,
                                  stdout=subprocess.PIPE, check=True).stdout.splitlines()[0]
        flags = {name: cache.get(name) for name in ('CMAKE_BUILD_TYPE', 'CMAKE_CXX_FLAGS',
                 'CMAKE_CXX_FLAGS_RELEASE', 'CAPSTAN_NO_EXCEPTIONS',
                 'CAPSTAN_NO_EXCEPTION_STRICT', 'CAPSTAN_FAST_RUNNER', 'CAPSTAN_ERASED_INVOCATIONS',
                 'CAPSTAN_UV_WORKER', 'CAPSTAN_SHA256_BACKEND', 'CAPSTAN_SHA256_PROVIDER', 'CAPSTAN_ASAN', 'CAPSTAN_TSAN', 'CAPSTAN_LTO', 'CAPSTAN_NATIVE_ARCH')}
        cpu_info = Path('/proc/cpuinfo')
        cpu_match = re.search(r'^model name\s*:\s*(.+)$', cpu_info.read_text(), re.M) if cpu_info.exists() else None
        cpu = cpu_match[1] if cpu_match else None
        if platform.system() == 'Darwin':
            probe = subprocess.run(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            if probe.returncode == 0:
                cpu = probe.stdout.strip()
        report = dict(environment=dict(system=platform.system(), architecture=platform.machine(),
                      cpu=cpu, compiler=compiler, build=flags),
                      phase_seconds=phase_seconds, diagnostics=diagnostics,
                      observed_close_to_activation_seconds=float(downtime[1]), host_peak_rss_kib=peaks, passed=True, native_bytes=args.bytes, package_bytes=int(measurement[1]),
                      end_to_end_seconds=float(measurement[2]), MiB_per_second=float(measurement[3]),
                      scope='One end-to-end demo sample, including reservation, Drain, transfer, activation and cleanup; not a throughput benchmark.')
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    finally:
        for host in hosts:
            host.terminate()
        for host in hosts:
            try:
                host.wait(timeout=5)
            except subprocess.TimeoutExpired:
                host.kill()
                host.wait()
        for reader in readers:
            reader.join(timeout=5)
        for log in logs:
            log.close()
        config.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
