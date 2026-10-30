#!/bin/bash
# Prepare offline inputs on Linux. No SmartOS image or zone is modified.
set -euo pipefail
fixture_dir=${1:?usage: prepare-fixture.sh FIXTURE_DIRECTORY}
node_source=/opt/node-v24.19.0-linux-x64
test -d "$fixture_dir/cache/_cacache"
test -f "$fixture_dir/control-prefix/node_modules/@openai/codex/package.json"
test -x "$node_source/bin/node"
test "$("$node_source/bin/node" --version)" = v24.19.0

runtime_dir=$fixture_dir/runtime
mkdir -p "$runtime_dir/bin" "$runtime_dir/lib/node_modules"
cp -a "$node_source/bin/node" "$node_source/bin/npm" "$node_source/bin/npx" \
    "$runtime_dir/bin/"
cp -L /usr/bin/env "$runtime_dir/bin/env"
cp -L /bin/dash "$runtime_dir/bin/sh"
cp -a "$node_source/lib/node_modules/npm" "$runtime_dir/lib/node_modules/"
for library in libdl.so.2 libstdc++.so.6 libm.so.6 libgcc_s.so.1 \
    libpthread.so.0 libc.so.6; do
    cp -L "/lib/x86_64-linux-gnu/$library" "$runtime_dir/lib/$library"
done
cp -L /lib64/ld-linux-x86-64.so.2 "$runtime_dir/lib/ld-linux-x86-64.so.2"

LD_LIBRARY_PATH="$runtime_dir/lib" "$runtime_dir/bin/node" --version
PATH="$runtime_dir/bin:$PATH" LD_LIBRARY_PATH="$runtime_dir/lib" \
    "$runtime_dir/bin/npm" --version
tar -C "$runtime_dir" -czf "$fixture_dir/node-npm-runtime.tar.gz" .
tar -C "$fixture_dir/cache" -cf "$fixture_dir/npm-cache.tar" .
(
    cd "$fixture_dir"
    sha256sum node-npm-runtime.tar.gz npm-cache.tar > payload.sha256
)
python3 - "$fixture_dir" <<'PY'
import datetime
import json
import pathlib
import sys

fixture = pathlib.Path(sys.argv[1])
manifest = json.loads((fixture / "control-prefix/node_modules/@openai/codex/package.json").read_text())
provenance = {
    "recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "codex_version": manifest["version"],
    "node_version": "24.19.0",
    "npm_version": "11.17.0",
    "registry": "https://registry.npmjs.org/",
    "linux_control": "npm install --offline --prefix control-prefix --no-audit --no-fund @openai/codex@0.166.0",
    "linux_control_result": "installed 2 packages; codex-cli 0.166.0",
    "lx_install_result": "not yet tested",
    "isolation_strategy": "no choice implied by preparing installer inputs",
    "host_deployment": False,
}
(fixture / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
PY
cat "$fixture_dir/payload.sha256"
