#!/usr/bin/env python3
"""Verified private runtime launcher; trusted bundle, no concurrent modification."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

# Replaced when the reviewed runtime is assembled; not supplied by the caller.
EXPECTED_MANIFEST_SHA256 = '@MANIFEST_SHA256@'


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda: stream.read(1048576), b''):
            h.update(data)
    return h.hexdigest()


def verify(root):
    manifest = root / 'runtime.json'
    if manifest.is_symlink() or digest(manifest) != EXPECTED_MANIFEST_SHA256:
        raise ValueError('runtime manifest identity mismatch')
    description = json.loads(manifest.read_text())
    files = description['files']
    expected = {}
    for name, checksum in files.items():
        rel = Path(name)
        if rel.is_absolute() or '..' in rel.parts:
            raise ValueError('invalid runtime manifest path')
        p = root / rel
        if p.is_symlink() or p.resolve() != p or not p.is_file():
            raise ValueError('runtime file is not a direct regular file: ' + name)
        if digest(p) != checksum:
            raise ValueError('runtime file identity mismatch: ' + name)
        expected[str(p)] = checksum
    binary = root / 'zdb-inventory'
    loader = root / 'ld.so.1'
    if str(binary) not in expected or str(loader) not in expected:
        raise ValueError('runtime manifest lacks executable or loader')
    # No inherited LD_*, SPA_CONFIG_PATH, tunables, locale or other settings.
    env = {'PATH': '/usr/bin:/usr/sbin:/bin', 'LANG': 'C', 'LC_ALL': 'C',
           'LD_LIBRARY_PATH_64': str(root / 'lib'), 'LD_NOCONFIG': '1',
           'LD_NOENVCONFIG': '1', 'LD_BIND_NOW': '1', 'LD_NOLAZYLOAD': '1'}
    trace = subprocess.run([str(loader), '-e', 'LD_TRACE_LOADED_OBJECTS_E=1', str(binary)],
                           env=env,
                           text=True, capture_output=True, timeout=30)
    if trace.returncode != 0:
        raise ValueError('private loader dependency check failed')
    resolved = set()
    for line in trace.stdout.splitlines():
        match = re.fullmatch(r'\s*([^\s]+)\s*=>\s*(/[^\s]+)\s*', line)
        if match is None:
            raise ValueError('unexpected dependency trace: ' + line)
        name, path = match.groups()
        if path not in expected or Path(path).parent != root / 'lib':
            raise ValueError('dependency escaped verified runtime: ' + name)
        if Path(path).name != name:
            raise ValueError('unexpected dependency alias: ' + name)
        resolved.add(path)
    required = {str(root / n) for n in description['dependency_closure']}
    if not required.issubset(expected):
        raise ValueError('unhashed dependency in closure')
    if resolved != required:
        raise ValueError('runtime dependency closure mismatch')
    return loader, binary, env


def main():
    root = Path(__file__).resolve().parent
    try:
        loader, binary, env = verify(root)
        if sys.argv[1:] == ['--verify-only']:
            print('Exact executable, loader and dependency closure verified')
            return 0
        os.execve(str(loader), [str(loader), str(binary), *sys.argv[1:]], env)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print('Refusing inventory launch: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
