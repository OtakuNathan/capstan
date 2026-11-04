#!/usr/bin/env python3
"""Export locked commits into the build tree without touching source checkouts."""
import argparse
import io
import json
import hashlib
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import shutil

p = argparse.ArgumentParser()
p.add_argument('--lock', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--lease', required=True)
p.add_argument('--dynabridge', required=True)
p.add_argument('--flux_foundry', required=True)
a = p.parse_args()
for dep in json.loads(a.lock.read_text())['dependencies']:
    name, commit = dep['name'], dep['commit']
    source = getattr(a, name)
    destination = a.output / name
    marker = destination / '.capstan-commit'
    patches = dep.get('patches', [])
    for patch in patches:
        path = a.lock.resolve().parent / patch['file']
        if hashlib.sha256(path.read_bytes()).hexdigest() != patch['sha256']:
            raise SystemExit(f'Patch digest mismatch: {path}')
    if marker.exists() and marker.read_text().strip() == commit:
        print(f'{name}: cached locked snapshot {commit}')
    else:
        if destination.exists():
            raise SystemExit(f'Refusing to overwrite unrecognized snapshot: {destination}')
        actual = subprocess.check_output(['git', '-C', source, 'rev-parse', commit + '^{commit}'], text=True).strip()
        if actual != commit:
            raise SystemExit(f'Commit mismatch: {name}')
        data = subprocess.check_output(['git', '-C', source, 'archive', '--format=tar', commit])
        destination.mkdir(parents=True)
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            archive.extractall(destination, filter='data')
        marker.write_text(commit + '\n')
        print(f'{name}: exported {commit} from {Path(source).resolve()}')
    applied = destination / '.capstan-patches.json'
    if applied.exists():
        if json.loads(applied.read_text()) != patches:
            raise SystemExit(f'Patch set changed; select a fresh build directory: {destination}')
        continue
    if patches:
        env = os.environ.copy()
        env['GIT_CEILING_DIRECTORIES'] = str(destination.resolve().parent)
        paths = [str((a.lock.resolve().parent / patch['file']).resolve()) for patch in patches]
        # Patches may build on files introduced by an earlier patch. Validate
        # and apply in order on staging; failed chains leave the export intact.
        with tempfile.TemporaryDirectory(prefix='.capstan-patches-', dir=a.output) as temporary:
            staged=Path(temporary)/'snapshot'
            shutil.copytree(destination,staged)
            for path in paths:
                subprocess.run(['git', 'apply', '--check', path], cwd=staged, env=env, check=True)
                subprocess.run(['git', 'apply', path], cwd=staged, env=env, check=True)
            (staged/'.capstan-patches.json').write_text(json.dumps(patches, indent=2) + '\n')
            backup=Path(temporary)/'original'
            destination.rename(backup)
            try:
                staged.rename(destination)
            except BaseException:
                backup.rename(destination)
                raise
        print(f'{name}: applied {len(patches)} SHA256-locked patch(es) to build snapshot')
