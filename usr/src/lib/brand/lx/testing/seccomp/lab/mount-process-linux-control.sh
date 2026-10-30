#!/bin/bash
set -euo pipefail
test "$(id -u)" = 0
work=$(mktemp -d /var/tmp/lx-mount-proc-control.XXXXXX)
chmod 0755 "$work"
mkdir -p "$work"/{static,dynamic}
cp /home/jwagner/lx-codex-tests/mount-process/mount_proc "$work/static/"
cp /home/jwagner/lx-codex-tests/mount-process/dynamic/mount_proc "$work/dynamic/"
job=
orphan=
cleanup() {
    test -z "$job" || kill -KILL "$job" 2>/dev/null || true
    test -z "$orphan" || kill -KILL "$orphan" 2>/dev/null || true
}
trap cleanup EXIT
wait_marker() {
    local file=$1 marker=$2 i
    for i in {1..200}; do
        grep -q "$marker" "$file" && return 0
        sleep 0.05
    done
    cat "$file"
    return 1
}
wait_stopped() {
    local pid=$1 i
    for i in {1..200}; do
        test "$(ps -p "$pid" -o stat= | cut -c1)" = T && return 0
        sleep 0.05
    done
    return 1
}
for variant in static dynamic; do
    binary=$work/$variant/mount_proc
    for uid in 0 65534; do
        fixture=$work/fixture-$variant-$uid
        mkdir -p "$fixture"/{cwd,covered}
        chmod 0755 "$fixture" "$fixture/cwd" "$fixture/covered"
        printf 'original\n' > "$fixture/covered/original"
        printf 'source\n' > "$fixture/source"
        printf 'target\n' > "$fixture/bind-target"
        touch "$fixture/result"
        chown "$uid:$uid" "$fixture/result"
        chmod 0600 "$fixture/result"
        chmod 0666 "$fixture/source" "$fixture/bind-target"
        args=()
        test "$uid" = 0 || args+=(--drop)
        log=$fixture/run.log
        "$binary" --base "$fixture" --linux "${args[@]}" > "$log" 2>&1 &
        job=$!
        wait_marker "$log" '^MP_ZOMBIE_PID='
        wait_stopped "$job"
        zombie=$(sed -n 's/^MP_ZOMBIE_PID=//p' "$log")
        test "$(ps -p "$zombie" -o stat= | cut -c1)" = Z
        "$binary" --base "$fixture" --outside "${args[@]}"
        kill -CONT "$job"
        wait_marker "$log" '^MP_ORPHAN_PID='
        orphan=$(sed -n 's/^MP_ORPHAN_PID=//p' "$log")
        wait "$job"
        job=
        wait_stopped "$orphan"
        kill -CONT "$orphan"
        wait_marker "$fixture/result" '^MP_CHILD_PASS mode=2 .*failures=0$'
        orphan=
        grep -qx "MP_PARENT failures=0 uid=$uid" "$log"
        grep -qx "MP_CHILD_SIGNAL_READY uid=$uid failures=0" "$fixture/result"
        "$binary" --base "$fixture" --outside "${args[@]}"
        printf 'LINUX_MOUNT_PROCESS_PASS variant=%s uid=%s\n' "$variant" "$uid"
    done
done
echo "LINUX_MOUNT_PROCESS_EVIDENCE=$work"
