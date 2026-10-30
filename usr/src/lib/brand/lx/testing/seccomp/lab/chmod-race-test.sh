#!/bin/bash
set -euo pipefail
export PATH=/lab:/usr/sbin:/usr/bin:/sbin:/bin
bootparams | grep -q '^lx_lab=true$'
test "$(zonename)" = global
probe_job= tracer_job= racer_job= target_pid= path= stop=
cleanup() {
    test -z "$stop" || touch "$stop"
    if test -n "$tracer_job"; then kill -TERM "$tracer_job" 2>/dev/null || true; wait "$tracer_job" || true; fi
    if test -n "$target_pid"; then kill -KILL "$target_pid" 2>/dev/null || true; fi
    if test -n "$probe_job"; then wait "$probe_job" || true; fi
    if test -n "$racer_job"; then wait "$racer_job" || true; fi
    test -z "$path" || rm -f "$path"
}
trap cleanup EXIT
for probe in /test/scope_ids /test/dynamic/scope_ids; do
    tag=$(echo "$probe" | tr / _)
    log=/lab/mode-race$tag.log
    trace=/lab/mode-race$tag.trace
    stop=/lab/mode-race$tag.stop
    rm -f "$stop"
    : > "$log"
    zlogin -S lx-probe "$probe --chmod-race" > "$log" 2>&1 &
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
    armed=
    for attempt in {1..10}; do
        armed=$(sed -n 's/^CHMOD_RACE_ARM_PID=//p' "$log")
        test -z "$armed" || break
        sleep 1
    done
    [[ "$armed" = "$target_pid" ]]
    path=/zones/lx-probe-zfs/root/tmp/lx-chmod-race-$target_pid
    # Delay the early policy return, outside the ZFS update locks.
    cat > /lab/chmod-race.d <<DSCRIPT
BEGIN { printf("CHMOD_TRACE_READY\n"); }
fbt:zfs:zfs_setattr:entry /pid == $target_pid/ { self->granted=0; self->stale=0; }
fbt:unix:brand_cred_vnode_setattr:entry /pid == $target_pid/ {
    self->uid=((vattr_t *)arg3)->va_uid; self->gid=((vattr_t *)arg3)->va_gid;
}
fbt:genunix:secpolicy_vnode_setattr:entry /pid == $target_pid/ {
    self->eligible=((vattr_t *)arg3)->va_uid == 1001;
}
fbt:genunix:secpolicy_vnode_setattr:return /pid == $target_pid && self->eligible && arg1 == 0 && self->delays < 10/ {
    self->granted=1; self->delays++;
    printf("SCOPED_CHMOD_EARLY_GRANT delay=%u\n", self->delays);
    chill(2000000 + (self->delays % 7) * 1000000);
}
fbt:unix:brand_cred_vnode_setattr:return /pid == $target_pid && self->granted && self->uid == 2000 && self->gid == 2000/ {
    printf("SCOPED_CHMOD_STALE_CHECK uid=%u gid=%u result=%d\n",self->uid,self->gid,arg1);
    if(arg1 == 1) { self->stale=1; }
    else { printf("SCOPED_CHMOD_STALE_VIOLATION\n"); exit(1); }
}
fbt:zfs:zfs_setattr:return /pid == $target_pid && self->stale/ {
    printf("SCOPED_CHMOD_STALE_RETURN result=%d\n",arg1);
    if(arg1 == 1) { printf("SCOPED_CHMOD_STALE_DENIED\n"); }
    else { printf("SCOPED_CHMOD_STALE_VIOLATION\n"); exit(1); }
}
DSCRIPT
    dtrace -x nolibs -w -q -s /lab/chmod-race.d > "$trace" 2>&1 &
    tracer_job=$!
    for attempt in {1..10}; do
        grep -q CHMOD_TRACE_READY "$trace" && break
        kill -0 "$tracer_job"
        sleep 1
    done
    grep -q CHMOD_TRACE_READY "$trace"
    /lab/chown-racer-native "$path" "$stop" > /lab/mode-racer$tag.log 2>&1 &
    racer_job=$!
    sleep 1
    kill -CONT "$target_pid"
    done_probe=0
    for attempt in {1..15}; do
        if ! kill -0 "$probe_job" 2>/dev/null; then done_probe=1; break; fi
        sleep 1
    done
    test "$done_probe" = 1
    wait "$probe_job"; probe_job=; target_pid=
    touch "$stop"
    wait "$racer_job"; racer_job=
    # DTrace preserves the ignored SIGINT inherited by background jobs.
    kill -TERM "$tracer_job"
    wait "$tracer_job"; tracer_job=
    cat "$log" "$trace" /lab/mode-racer$tag.log
    grep -q CHMOD_RACE_PROBE_PASS "$log"
    grep -q SCOPED_CHMOD_STALE_DENIED "$trace"
    ! grep -q SCOPED_CHMOD_STALE_VIOLATION "$trace"
    ! grep -q '^dtrace:' "$trace"
    rm -f "$path"; path=
done
trap - EXIT
echo LX_CHMOD_RACE_PASS
