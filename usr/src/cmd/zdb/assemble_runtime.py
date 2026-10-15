#!/usr/bin/env python3
"""Builder-local reproducible assembly; destination must not already exist."""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

base = Path('/build/workspaces/raidz')
build = base / 'inventory-build'
source = base / 'inventory-prototype/usr/src/cmd/zdb'
dest = Path(sys.argv[1]).absolute()
dest.mkdir()
(dest / 'lib').mkdir()
env = {'PATH': '/usr/bin:/usr/sbin:/bin', 'LANG': 'C',
       'LD_LIBRARY_PATH_64': ':'.join(map(str, [
           build / 'lib', base / 'smartos-live/proto/usr/lib/amd64',
           base / 'smartos-live/proto/lib/amd64']))}
trace = subprocess.check_output(['/usr/bin/ldd', str(build / 'zdb-inventory')],
                                env=env, text=True)
for line in trace.splitlines():
    match = re.fullmatch(r'\s*(\S+)\s*=>\s*(/\S+)\s*', line)
    if not match:
        raise ValueError('unresolved dependency: ' + line)
    name, path = match.groups()
    shutil.copy2(path, dest / 'lib' / name)
shutil.copy2(build / 'zdb-inventory', dest / 'zdb-inventory')
shutil.copy2(base / 'smartos-live/proto/lib/amd64/ld.so.1', dest / 'ld.so.1')

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

files = {str(p.relative_to(dest)): sha(p) for p in dest.rglob('*') if p.is_file()}
env = {'PATH': '/usr/bin:/usr/sbin:/bin', 'LANG': 'C', 'LC_ALL': 'C',
       'LD_LIBRARY_PATH_64': str(dest / 'lib'), 'LD_NOCONFIG': '1',
       'LD_NOENVCONFIG': '1', 'LD_BIND_NOW': '1', 'LD_NOLAZYLOAD': '1'}
trace = subprocess.check_output([str(dest / 'ld.so.1'), '-e',
                                'LD_TRACE_LOADED_OBJECTS_E=1',
                                str(dest / 'zdb-inventory')], env=env, text=True)
closure = []
for line in trace.splitlines():
    match = re.fullmatch(r'\s*(\S+)\s*=>\s*(/\S+)\s*', line)
    if not match:
        raise ValueError('unresolved private dependency: ' + line)
    rel = str(Path(match.group(2)).relative_to(dest))
    if rel not in files:
        raise ValueError('dependency outside hashed set')
    closure.append(rel)
(dest / 'runtime.json').write_text(json.dumps(
    {'files': files, 'dependency_closure': closure}, indent=2))
(dest / 'SHA256SUMS').write_text(''.join(
    v + '  ' + k + '\n' for k, v in sorted(files.items())))
(dest / 'DEPENDENCIES').write_text('\n'.join(sorted(closure)) + '\n')
py = (source / 'launch_inventory.py').read_text().replace(
    '@MANIFEST_SHA256@', sha(dest / 'runtime.json'))
sh = (source / 'launch_inventory.sh').read_text().replace(
    '@HASHLIST_SHA256@', sha(dest / 'SHA256SUMS')).replace(
    '@CLOSURE_SHA256@', sha(dest / 'DEPENDENCIES'))
(dest / 'launch.py').write_text(py)
(dest / 'launch.sh').write_text(sh)
subprocess.run(['/bin/bash', str(dest / 'launch.sh'), '--verify-only'], check=True)
print(dest)
