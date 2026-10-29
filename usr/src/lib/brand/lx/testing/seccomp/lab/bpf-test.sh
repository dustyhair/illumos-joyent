#!/bin/bash
# Run only in this thread's disposable offline SmartOS VM.
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
test -f /lab/offline-lx-guest
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
echo LX_BPF_START
digest -a sha256 /kernel/brand/amd64/lx_brand /lab/lx_seccomp_ktest /lab/linux-vectors.bin
modload /lab/lx_seccomp_ktest
ktest list lx_seccomp:bpf:vectors
ktest run -i /lab/linux-vectors.bin lx_seccomp:bpf:vectors
for bad in truncated trailing header; do
    set +e
    ktest run -i "/lab/bpf-bad-${bad}.bin" lx_seccomp:bpf:vectors
    rc=$?
    set -e
    echo "LX_BPF_BAD_${bad}_EXIT=$rc"
    test "$rc" -eq 1
done
echo LX_BPF_PASS
