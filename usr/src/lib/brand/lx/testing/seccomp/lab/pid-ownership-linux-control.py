import os, pathlib, re, signal, subprocess, time

base = pathlib.Path('/var/tmp/lx-pid-ownership-controls')

def marker(log, pattern, child):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        content = log.read_text()
        found = re.search(pattern, content, re.M)
        if found:
            return found
        if child.poll() is not None:
            break
        time.sleep(.03)
    raise AssertionError((pattern, log.read_text()))

def state(pid, wanted):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            text = pathlib.Path(f'/proc/{pid}/status').read_text()
        except FileNotFoundError:
            text = ''
        if f'\nState:\t{wanted}' in text:
            return
        time.sleep(.03)
    raise AssertionError((pid, wanted, text))

for variant in ['static', 'dynamic']:
    for uid in [0, 65534]:
        for guard in [0, 1]:
            for mode in ['exit', 'exec', 'fork']:
                name = f'{variant}-{uid}-{guard}-{mode}'
                log = base / (name + '.log')
                args = ['aa-exec', '-p', 'lxc-unshare', '--', str(base / variant), mode]
                if uid:
                    args += ['--drop']
                if guard:
                    args += ['--guard']
                with log.open('w') as out:
                    child = subprocess.Popen(args, stdout=out, stderr=subprocess.STDOUT)
                    descendant = None
                    try:
                        found = marker(log, r'^PID_PROCESS pid=(\d+) tid=(\d+)', child)
                        pid = int(found[1])
                        assert found[1] == found[2]
                        state(pid, 'T')
                        os.kill(pid, signal.SIGCONT)
                        if mode == 'fork':
                            found = marker(log, r'^PID_PARENT pid=(\d+) child=(\d+)', child)
                            assert int(found[1]) == pid
                            descendant = int(found[2])
                            other = marker(log, r'^PID_CHILD pid=(\d+) parent=(\d+)', child)
                            assert int(other[1]) == descendant and int(other[2]) == pid
                            state(pid, 'T')
                            state(descendant, 'T')
                            os.kill(descendant, signal.SIGCONT)
                            state(descendant, 'Z')
                            os.kill(pid, signal.SIGCONT)
                        else:
                            found = marker(log, r'^PID_WORKER pid=(\d+) tid=(\d+)', child)
                            assert int(found[1]) == pid and found[1] != found[2]
                            state(pid, 'T')
                            os.kill(pid, signal.SIGCONT)
                        found = marker(log, r'^PID_RETIRED pid=(\d+) tid=(\d+)', child)
                        assert int(found[1]) == pid and found[1] == found[2]
                        state(pid, 'T')
                        os.kill(pid, signal.SIGCONT)
                        assert child.wait(timeout=10) == 0, (name, log.read_text())
                        assert log.read_text().endswith('PID_PASS\n')
                        if mode == 'fork':
                            assert not pathlib.Path(f'/proc/{descendant}').exists()
                        print('LINUX_PID_OWNERSHIP_PASS', name, flush=True)
                    finally:
                        if child.poll() is None:
                            if descendant:
                                try:
                                    content = pathlib.Path(f'/proc/{descendant}/status').read_text()
                                    if re.search(r'^PPid:\s+' + str(child.pid) + '$', content, re.M):
                                        os.kill(descendant, signal.SIGKILL)
                                except ProcessLookupError:
                                    pass
                                except FileNotFoundError:
                                    pass
                            child.kill()
                        child.wait()
