import subprocess,time,pathlib,os,signal,re
base=pathlib.Path('/var/tmp/lx-pid-identity-controls')
for variant in ['static','dynamic']:
 for uid in [0,65534]:
  for guard in [False,True]:
   for mode in ['exit','exec']:
    name=f'{variant}-{uid}-{int(guard)}-{mode}';log=base/(name+'.log')
    args=['aa-exec','-p','lxc-unshare','--',str(base/variant),mode]
    if uid:args+=['--drop']
    if guard:args+=['--guard']
    with log.open('w') as out:
     child=subprocess.Popen(args,stdout=out,stderr=subprocess.STDOUT)
     try:
      for marker in ['PID_WORKER','PID_RETIRED']:
       deadline=time.monotonic()+15
       while True:
        s=log.read_text();m=re.search(marker+r' pid=(\d+) tid=(\d+)',s)
        if m and '\nState:\tT' in pathlib.Path(f'/proc/{m[1]}/status').read_text():break
        if child.poll() is not None or time.monotonic()>deadline:raise AssertionError((name,marker,s))
        time.sleep(.05)
       if marker=='PID_WORKER':assert m[1]!=m[2]
       else:assert m[1]==m[2]
       os.kill(int(m[1]),signal.SIGCONT)
      assert child.wait(timeout=10)==0,(name,log.read_text())
      assert 'PID_PASS' in log.read_text()
      print('LINUX_PID_IDENTITY_PASS',name,flush=True)
     finally:
      if child.poll() is None:child.kill();child.wait()
