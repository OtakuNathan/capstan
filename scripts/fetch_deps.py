#!/usr/bin/env python3
"""Explicitly fetch exact dependency commits into isolated local checkouts."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build-deps')
    args = parser.parse_args()
    for dependency in json.loads((ROOT / 'deps.lock.json').read_text())['dependencies']:
        name, commit = dependency['name'], dependency['commit']
        destination = args.output.resolve() / name
        if destination.exists():
            head = subprocess.check_output(['git', '-C', str(destination), 'rev-parse', 'HEAD'], text=True).strip()
            if head != commit:
                raise SystemExit(f'Refusing to modify existing checkout {destination}; choose a fresh --output')
        else:
            destination.mkdir(parents=True)
            subprocess.run(['git', 'init', '-q', str(destination)], check=True)
            subprocess.run(['git', '-C', str(destination), 'remote', 'add', 'origin', dependency['repository']], check=True)
            subprocess.run(['git', '-C', str(destination), 'fetch', '--depth=1', 'origin', commit], check=True)
            subprocess.run(['git', '-C', str(destination), 'checkout', '--detach', 'FETCH_HEAD'], check=True)
        actual = subprocess.check_output(['git', '-C', str(destination), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != commit:
            raise SystemExit(f'Commit mismatch: {name}')
        print(f'{name}: {commit} in {destination}', flush=True)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
