import ctypes
import os
from pathlib import Path
import re
import signal
import subprocess
import time

base = Path('/var/tmp/lx-pid-ownership-controls')
# Adopt the fixture's orphans so this driver can reap every process it starts.
assert ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) == 0

def children():
    return [int(x) for x in Path(f'/proc/self/task/{os.getpid()}/children').read_text().split()]

def stopped(pid):
    return '\nState:\tT' in Path(f'/proc/{pid}/status').read_text()

def cleanup():
    deadline = time.monotonic() + 5
    while children():
        assert time.monotonic() < deadline, children()
        for pid in children():
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        while True:
            try:
                if os.waitpid(-1, os.WNOHANG)[0] == 0:
                    break
            except ChildProcessError:
                break
        time.sleep(.01)

for variant in ('static', 'dynamic'):
    for uid in (0, 65534):
        for guard in (0, 1):
            label = f'{variant}-{uid}-{guard}'
            log = base / f'cohort-negative-{label}.log'
            args = ['aa-exec', '-p', 'lxc-unshare', '--', str(base / variant), 'death']
            if uid:
                args.append('--drop')
            if guard:
                args.append('--guard')
            with log.open('w') as out:
                runner = subprocess.Popen(args, stdout=out, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + 10
                    while True:
                        text = log.read_text()
                        main = re.search(r'^PID_PROCESS pid=(\d+) ', text, re.M)
                        if main and stopped(int(main[1])):
                            break
                        assert runner.poll() is None and time.monotonic() < deadline, text
                        time.sleep(.01)
                    assert int(main[1]) == runner.pid
                    os.kill(runner.pid, signal.SIGCONT)
                    while True:
                        text = log.read_text()
                        child = re.search(r'^PID_CHILD pid=(\d+) parent=(\d+) descendant=(\d+)', text, re.M)
                        descendant = re.search(r'^PID_DESCENDANT pid=(\d+) parent=(\d+)', text, re.M)
                        if child and descendant and all(stopped(pid) for pid in (runner.pid, int(child[1]), int(descendant[1]))):
                            break
                        assert runner.poll() is None and time.monotonic() < deadline, text
                        time.sleep(.01)
                    assert child[2] == main[1] and child[3] == descendant[1] and child[1] == descendant[2]
                    runner.kill()
                    assert runner.wait(timeout=5) == -signal.SIGKILL
                    time.sleep(.1)
                    # No PID namespace here: killing the parent must leave both alive.
                    assert stopped(int(child[1])) and stopped(int(descendant[1]))
                    print('LINUX_COHORT_NEGATIVE_PASS', label, flush=True)
                finally:
                    if runner.poll() is None:
                        runner.kill()
                    runner.wait()
                    cleanup()
