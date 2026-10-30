#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
probe_job= tracer_job= target_pid= path=
cleanup() {
    if test -n "$tracer_job"; then kill -TERM "$tracer_job" 2>/dev/null || true; wait "$tracer_job" || true; fi
    if test -n "$target_pid"; then kill -KILL "$target_pid" 2>/dev/null || true; fi
    if test -n "$probe_job"; then wait "$probe_job" || true; fi
    test -z "$path" || rm -f "$path"
}
trap cleanup EXIT
await_marker() {
    local marker=$1 value
    for attempt in {1..10}; do
        value=$(sed -n "s/^$marker=//p" "$log")
        test "$value" != "$target_pid" || return 0
        kill -0 "$probe_job" 2>/dev/null || break
        sleep 1
    done
    cat "$log"
    return 1
}
for probe in /test/scope_ids /test/dynamic/scope_ids; do
    tag=$(echo "$probe" | tr / _)
    log=/lab/mode-acl$tag.log
    trace=/lab/mode-acl$tag.trace
    zlogin -S lx-probe "$probe --chmod-acl" > "$log" 2>&1 &
    probe_job=$!
    target_pid=
    for attempt in {1..10}; do
        target_pid=$(sed -n 's/^SCOPE_READY_PID=//p' "$log")
        test -z "$target_pid" || break
        sleep 1
    done
    [[ "$target_pid" =~ ^[0-9]+$ ]]
    printf '%s\0' "$target_pid" > /lab/scope-target.bin
    /lab/ktest run -i /lab/scope-target.bin lx_namespace:user:scope_attach
    kill -CONT "$target_pid"
    await_marker CHMOD_ACL_ARM_PID
    path=/zones/lx-probe-zfs/root/tmp/lx-chmod-acl-$target_pid
    chmod A0+user:1000:write_acl:allow "$path"
    chown 2000:2000 "$path"
    /lab/chown-racer-native --acl-mode-control "$path"
    chown 2000:2000 "$path"
    chmod A0+user:1000:write_acl:allow "$path"
    ls -Vn "$path"
    cat > /lab/chmod-acl.d <<DSCRIPT
BEGIN { printf("CHMOD_ACL_TRACE_READY\n"); }
fbt:zfs:zfs_setattr:entry /pid == $target_pid/ { self->policies=0; self->kind=0; }
fbt:genunix:secpolicy_vnode_setattr:entry /pid == $target_pid/ { self->policies++; }
fbt:unix:brand_cred_vnode_setattr:entry /pid == $target_pid/ {
    self->uid=((vattr_t *)arg3)->va_uid; self->gid=((vattr_t *)arg3)->va_gid;
}
fbt:unix:brand_cred_vnode_setattr:return /pid == $target_pid && self->policies == 0/ {
    if(self->uid == 2000 && self->gid == 2000 && arg1 == 1) { self->kind=1; }
    else if(self->uid == 1001 && self->gid == 1001 && arg1 == 0) { self->kind=2; }
    else if(self->uid == 1001 && self->gid == 1001 && arg1 == 1) { self->kind=3; }
    else { printf("SCOPED_CHMOD_ACL_VIOLATION uid=%u gid=%u result=%d\n",self->uid,self->gid,arg1); exit(1); }
}
fbt:zfs:zfs_setattr:return /pid == $target_pid && self->kind != 0/ {
    printf("SCOPED_CHMOD_ACL_RETURN kind=%u result=%d policies=%u\n",self->kind,arg1,self->policies);
    if((self->kind == 2 && arg1 != 0) || (self->kind != 2 && arg1 != 1)) {
        printf("SCOPED_CHMOD_ACL_VIOLATION\n"); exit(1);
    }
}
DSCRIPT
    dtrace -x nolibs -q -s /lab/chmod-acl.d > "$trace" 2>&1 &
    tracer_job=$!
    for attempt in {1..10}; do
        grep -q CHMOD_ACL_TRACE_READY "$trace" && break
        if ! kill -0 "$tracer_job"; then cat "$trace"; exit 1; fi
        sleep 1
    done
    grep -q CHMOD_ACL_TRACE_READY "$trace"
    kill -CONT "$target_pid"
    await_marker CHMOD_ACL_MAPPED_PID
    chown 1001:1001 "$path"
    kill -CONT "$target_pid"
    await_marker CHMOD_ACL_DROP_PID
    # chmod may discard the explicit ACE; restore it before the denial case.
    chmod A0+user:1000:write_acl:allow "$path"
    kill -CONT "$target_pid"
    done_probe=0
    for attempt in {1..10}; do
        if ! kill -0 "$probe_job" 2>/dev/null; then done_probe=1; break; fi
        sleep 1
    done
    test "$done_probe" = 1
    wait "$probe_job"; probe_job=; target_pid=
    kill -TERM "$tracer_job"
    wait "$tracer_job"; tracer_job=
    cat "$log" "$trace"
    grep -q CHMOD_ACL_PROBE_PASS "$log"
    grep -q 'SCOPED_CHMOD_ACL_RETURN kind=1 result=1 policies=0' "$trace"
    grep -q 'SCOPED_CHMOD_ACL_RETURN kind=2 result=0 policies=0' "$trace"
    grep -q 'SCOPED_CHMOD_ACL_RETURN kind=3 result=1 policies=0' "$trace"
    ! grep -q SCOPED_CHMOD_ACL_VIOLATION "$trace"
    ! grep -q '^dtrace:' "$trace"
    rm -f "$path"; path=
done
trap - EXIT
echo LX_CHMOD_ACL_PASS
