#!/usr/bin/env python3
import argparse
from pathlib import Path
import subprocess
from compile_policy import flags
p = argparse.ArgumentParser()
for flag in ('compiler', 'source', 'root', 'build', 'name'):
    p.add_argument('--' + flag, required=True)
a = p.parse_args()
b = Path(a.build)
includes = [a.root, str(Path(a.root) / 'include'), str(b / '_deps/lease/lease'), str(b / '_deps/portal'), str(b / '_deps/flux_foundry')]
command = [a.compiler, '-std=c++14', '-fsyntax-only'] + flags(b) + ['-I' + x for x in includes] + [a.source]
r = subprocess.run(command, capture_output=True, text=True)
log = b / 'compile-negative' / (a.name + '.log')
log.parent.mkdir(exist_ok=True)
log.write_text('COMMAND: ' + repr(command) + '\nEXIT: ' + str(r.returncode) + '\n' + r.stdout + r.stderr)
expected = {'exclusive_copy': 'deleted', 'readonly_write': 'const', 'wrong_receiver': 'conver', 'const_generated': 'const', 'async_return': 'conver', 'production_benchmark': 'benchmark_validation'}[a.name]
if r.returncode == 0 or expected not in r.stderr.lower():
    print(r.stdout + r.stderr)
    raise SystemExit('Negative probe did not fail for the expected reason: ' + expected)
print(f'PASS {a.name}: compiler exit={r.returncode}; diagnostic={expected}; {log}')
