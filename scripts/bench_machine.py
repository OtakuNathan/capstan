#!/usr/bin/env python3
"""Build, verify and benchmark the optimized native TCP path on Linux/macOS."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]


def capture(command, env=None):
    result = subprocess.run(command, env=env, cwd=ROOT, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return {'command': command, 'exit_code': result.returncode,
            'stdout': result.stdout.strip(), 'stderr': result.stderr.strip()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--quick', action='store_true', help='small validation batches; not comparable performance numbers')
    parser.add_argument('--jobs', type=int, default=2, help='parallel build jobs (default: 2)')
    parser.add_argument('--compiler', default=os.environ.get('CXX', 'clang++' if sys.platform == 'darwin' else 'c++'))
    parser.add_argument('--build-dir', type=Path, help='reusable CMake build directory')
    parser.add_argument('--output', type=Path, help='fresh result directory; default: timestamp under work/machine-bench')
    args = parser.parse_args()
    if sys.version_info < (3, 12):
        parser.error('Python 3.12+ is required; on macOS install Homebrew python and use its python3')
    if platform.system() not in ('Linux', 'Darwin'):
        parser.error('the native TCP benchmark currently supports Linux and macOS')
    if not 1 <= args.jobs <= 64:
        parser.error('jobs must be 1..64')
    env = os.environ.copy()
    brew = None
    if sys.platform == 'darwin':
        brew = shutil.which('brew')
        if not brew:
            candidates = ['/opt/homebrew/bin/brew', '/usr/local/bin/brew']
            if platform.machine() != 'arm64':
                candidates.reverse()
            brew = next((path for path in candidates if os.access(path, os.X_OK)), None)
    if brew:
        prefix = capture([brew, '--prefix'], env)
        if prefix['exit_code'] or not prefix['stdout']:
            parser.error('Homebrew prefix lookup failed: ' + prefix['stderr'])
        # An explicitly invoked Homebrew Python does not put Homebrew tools on PATH.
        env['PATH'] = os.pathsep.join([str(Path(prefix['stdout']) / folder) for folder in ('bin', 'sbin')]
                                     + [env.get('PATH', os.defpath)])
        prefixes = [capture([brew, '--prefix', formula], env) for formula in ('glib', 'libuv')]
        if any(p['exit_code'] for p in prefixes):
            parser.error('install dependencies first: brew install cmake pkgconf glib libuv python')
        pc_paths = [str(Path(p['stdout']) / folder) for p in prefixes for folder in ('lib/pkgconfig', 'share/pkgconfig')]
        env['PKG_CONFIG_PATH'] = os.pathsep.join(pc_paths + ([env['PKG_CONFIG_PATH']] if env.get('PKG_CONFIG_PATH') else []))
        env['CMAKE_PREFIX_PATH'] = os.pathsep.join([p['stdout'] for p in prefixes] + ([env['CMAKE_PREFIX_PATH']] if env.get('CMAKE_PREFIX_PATH') else []))
    compiler = shutil.which(args.compiler, path=env.get('PATH'))
    if not compiler:
        parser.error(f'compiler not found: {args.compiler}')
    for name in ('git', 'cmake', 'ctest'):
        if not shutil.which(name, path=env.get('PATH')):
            parser.error(f'missing {name}; see docs/benchmark-your-machine.md')
    pkg_config = (shutil.which('pkg-config', path=env.get('PATH')) or
                  shutil.which('pkgconf', path=env.get('PATH')))
    if not pkg_config and brew:
        # A formula can be installed without being linked into Homebrew's bin.
        prefix = capture([brew, '--prefix', 'pkgconf'], env)
        if prefix['exit_code'] == 0 and prefix['stdout']:
            for name in ('pkg-config', 'pkgconf'):
                pkg_config = shutil.which(str(Path(prefix['stdout']) / 'bin' / name))
                if pkg_config:
                    break
    if not pkg_config:
        parser.error('missing pkg-config/pkgconf; on macOS run: brew install pkgconf')
    env['PKG_CONFIG'] = pkg_config
    packages = capture([pkg_config, '--modversion', 'gio-2.0', 'libuv'], env)
    if packages['exit_code']:
        parser.error('GIO/libuv development files not found by pkg-config; see docs/benchmark-your-machine.md')
    translated = capture(['sysctl', '-in', 'sysctl.proc_translated']) if sys.platform == 'darwin' else None
    if translated and translated['stdout'] == '1':
        parser.error('Rosetta detected: use a native arm64 terminal, Python and Homebrew for the Apple Silicon run')
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    output = (args.output or ROOT / 'work/machine-bench' / (stamp + '-' + uuid.uuid4().hex[:8])).resolve()
    if output.exists() and any(output.iterdir()):
        parser.error('output directory must be empty; choose a new --output')
    output.mkdir(parents=True, exist_ok=True)
    build = (args.build_dir or ROOT / f'build-machine-{platform.system().lower()}-{platform.machine()}-{Path(compiler).name}').resolve()
    source_names = ['CMakeLists.txt', 'deps.lock.json', 'cmake/Transport.cmake',
                    'include/capstan/protocol/random.hpp', 'scripts/bench_machine.py']
    metadata = {'passed': False, 'profile': 'quick' if args.quick else 'full', 'started_utc': stamp,
                'system': platform.system(), 'architecture': platform.machine(),
                'platform': platform.platform(), 'python': sys.version,
                'compiler': capture([compiler, '--version'], env),
                'cmake': capture(['cmake', '--version'], env), 'packages': packages,
                'revision': capture(['git', 'rev-parse', 'HEAD'], env)['stdout'],
                'tracked_tree_dirty': capture(['git', 'diff', '--quiet', 'HEAD'], env)['exit_code'] != 0,
                'source_sha256': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in source_names},
                'build_dir': str(build), 'stages': [],
                'scope': 'One shared Dataset, loopback TCP, finite history; CPU, OS, compiler and libraries all affect cross-machine comparisons.'}
    if sys.platform == 'darwin':
        metadata['hardware'] = {key: capture(['sysctl', '-n', key])['stdout'] for key in
                                ('machdep.cpu.brand_string', 'hw.physicalcpu', 'hw.logicalcpu', 'hw.memsize')}
        metadata['rosetta_probe'] = translated
    else:
        cpuinfo = Path('/proc/cpuinfo').read_text() if Path('/proc/cpuinfo').exists() else ''
        metadata['hardware'] = {'cpu_names': sorted({line.split(':', 1)[1].strip() for line in cpuinfo.splitlines()
                                                     if line.split(':', 1)[0].strip() in ('model name', 'Hardware')}),
                                'logical_cpus': os.cpu_count()}
    def save():
        (output / 'machine.json').write_text(json.dumps(metadata, indent=2) + '\n')
    def run(label, command):
        print(f'[{label}] {" ".join(map(str, command))}', flush=True)
        with (output / (label + '.log')).open('w') as log:
            result = subprocess.run(list(map(str, command)), env=env, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        metadata['stages'].append({'name': label, 'command': list(map(str, command)), 'exit_code': result.returncode})
        save()
        if result.returncode:
            lines = (output / (label + '.log')).read_text(errors='replace').splitlines()
            print('\n'.join(lines[-40:]), file=sys.stderr)
            raise RuntimeError(f'{label} failed; full log: {output / (label + ".log")}')
    save()
    try:
        deps = ROOT / 'build-deps'
        run('fetch', [sys.executable, ROOT / 'scripts/fetch_deps.py', '--output', deps])
        options = ['-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG',
                   f'-DCMAKE_CXX_COMPILER={compiler}',
                   f'-DPython3_EXECUTABLE={sys.executable}', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
                   f'-DPKG_CONFIG_EXECUTABLE={pkg_config}',
                   '-DCAPSTAN_REQUIRE_GIO=ON', '-DCAPSTAN_UV_WORKER=ON', '-DCAPSTAN_FAST_RUNNER=ON',
                   '-DCAPSTAN_NO_EXCEPTIONS=ON', '-DCAPSTAN_NO_EXCEPTION_STRICT=ON',
                   '-DCAPSTAN_ASAN=OFF', '-DCAPSTAN_TSAN=OFF', '-DCAPSTAN_LTO=ON', '-DCAPSTAN_NATIVE_ARCH=ON',
                   f'-DCAPSTAN_LEASE_SOURCE={deps / "lease"}', f'-DCAPSTAN_DYNABRIDGE_SOURCE={deps / "dynabridge"}',
                   f'-DCAPSTAN_FF_SOURCE={deps / "flux_foundry"}']
        if sys.platform == 'linux' and 'clang' in metadata['compiler']['stdout'].lower() and shutil.which('ld.lld'):
            options.append('-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld')
        run('configure', ['cmake', '-S', ROOT, '-B', build, *options])
        run('build', ['cmake', '--build', build, '--parallel', args.jobs])
        if sys.platform == 'darwin':
            metadata['binary_architectures'] = capture(['lipo', '-archs', str(build / 'capstan_host')])
            if (metadata['binary_architectures']['exit_code'] or
                    platform.machine() == 'arm64' and 'arm64' not in metadata['binary_architectures']['stdout'].split()):
                raise RuntimeError('Host binary does not match native Apple Silicon; choose a fresh --build-dir')
        shutil.copyfile(build / 'CMakeCache.txt', output / 'CMakeCache.txt')
        shutil.copyfile(build / 'compile_commands.json', output / 'compile_commands.json')
        run('tests', ['ctest', '--test-dir', build, '--output-on-failure', '--timeout', '90'])
        for workload in ('version', 'summary'):
            levels = '1,4' if args.quick else ('1,2,4,8,16,32,48' if workload == 'version' else '1,8,32')
            run(workload, [sys.executable, ROOT / 'scripts/bench_concurrency.py', '--build-dir', build,
                           '--output', output / workload, '--workloads', workload, '--concurrency', levels,
                           '--rounds', 1 if args.quick else 3, '--samples', 64 if args.quick else 50000,
                           '--summary-samples', 16 if args.quick else 512,
                           '--warmup', 4 if args.quick else (1000 if workload == 'version' else 8),
                           '--history-limit', 65536, '--inline-version', 1, '--trace-requests', 0,
                           '--resource-bytes', 1048576 if args.quick else 67108864])
        reports = {name: json.loads((output / name / 'report.json').read_text()) for name in ('version', 'summary')}
        if not all(report['passed'] for report in reports.values()):
            raise RuntimeError('a workload report did not pass validation')
        lines = ['# Capstan machine benchmark', '', f'Profile: {metadata["profile"]}. Revision: `{metadata["revision"]}`.',
                 '', f'Platform: {metadata["platform"]}.', '',
                 'Throughput is the median batch rate; quantiles pool all rounds. Full results use a 64 MiB Dataset.',
                 'Quick results use small 1 MiB validation batches and are not comparable performance numbers.', '',
                 '| Workload | Connections | RPC/s median | RPC/s min–max | p50 µs | p95 µs | p99 µs |',
                 '|---|---:|---:|---:|---:|---:|---:|']
        for name, report in reports.items():
            for key, row in report['summary'].items():
                latency = row['latency']
                lines.append(f'| {name} | {key.split("/")[1]} | {row["throughput_median"]:.0f} | '
                             f'{row["throughput_min"]:.0f}–{row["throughput_max"]:.0f} | '
                             f'{latency["p50_us"]:.2f} | {latency["p95_us"]:.2f} | {latency["p99_us"]:.2f} |')
        lines += ['', 'See `machine.json`, `compile_commands.json`, `tests.log`, and each workload’s `report.json` and raw CSV files.',
                  'Results include the native driver and TCP stack; they are not isolated CPU comparisons or indefinite service capacity.', '']
        (output / 'SUMMARY.md').write_text('\n'.join(lines))
        metadata['passed'] = True
        print('\n'.join(lines), flush=True)
        print(f'Complete: {output / "SUMMARY.md"}', flush=True)
    except Exception as error:
        metadata['error'] = str(error)
        raise
    finally:
        save()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
