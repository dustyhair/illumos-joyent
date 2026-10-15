#!/bin/bash
# Generated hash pins; use only an independently reviewed, protected bundle.
set -euo pipefail
fail() { echo "Refusing inventory launch: $*" >&2; exit 2; }
root=$(cd -- "$(dirname -- "$0")" && pwd -P)
[[ $root =~ ^/[A-Za-z0-9_./-]+$ ]] || fail 'unsupported runtime directory characters'
check() {
    [[ -f "$1" && ! -L "$1" ]] || fail "not a regular runtime file: $1"
    [[ $(/usr/bin/digest -a sha256 "$1") == "$2" ]] || fail "identity mismatch: $1"
}
[[ -d "$root/lib" && ! -L "$root/lib" ]] || fail 'library directory must be direct'
check "$root/SHA256SUMS" '@HASHLIST_SHA256@'
check "$root/DEPENDENCIES" '@CLOSURE_SHA256@'
while read -r hash name extra; do
    [[ $hash =~ ^[a-f0-9]{64}$ && -z ${extra:-} ]] || fail 'invalid hash list'
    [[ $name =~ ^(lib/)?[A-Za-z0-9_.-]+$ && $name != . && $name != .. ]] || fail 'invalid runtime name'
    check "$root/$name" "$hash"
done < "$root/SHA256SUMS"
run_loader() {
    /usr/bin/env -i PATH=/usr/bin:/usr/sbin:/bin LANG=C LC_ALL=C \
        LD_LIBRARY_PATH_64="$root/lib" LD_NOCONFIG=1 LD_NOENVCONFIG=1 \
        LD_BIND_NOW=1 LD_NOLAZYLOAD=1 "$root/ld.so.1" "$@"
}
trace=$(run_loader -e LD_TRACE_LOADED_OBJECTS_E=1 "$root/zdb-inventory") || fail 'dependency trace failed'
actual=''
while read -r name arrow resolved extra; do
    [[ $arrow == '=>' && -z ${extra:-} && $resolved == "$root/lib/$name" ]] || fail 'dependency escaped runtime'
    [[ $name =~ ^[A-Za-z0-9_.-]+$ ]] || fail 'invalid dependency name'
    actual+="lib/$name"$'\n'
done <<< "$trace"
expected=$(/usr/bin/sort "$root/DEPENDENCIES")
[[ $(printf '%s' "$actual" | /usr/bin/sort) == "$expected" ]] || fail 'dependency closure mismatch'
if [[ $# == 1 && $1 == --verify-only ]]; then
    echo 'Exact executable, loader and dependency closure verified'
    exit 0
fi
exec /usr/bin/env -i PATH=/usr/bin:/usr/sbin:/bin LANG=C LC_ALL=C \
    LD_LIBRARY_PATH_64="$root/lib" LD_NOCONFIG=1 LD_NOENVCONFIG=1 \
    LD_BIND_NOW=1 LD_NOLAZYLOAD=1 "$root/ld.so.1" "$root/zdb-inventory" "$@"
