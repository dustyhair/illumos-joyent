// CDDL version 1.0, http://www.illumos.org/license/CDDL.
// Exercise immediate-exit terminal output through the real Codex executor.
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const readline = require('node:readline');
const { spawn } = require('node:child_process');

async function main() {
  const [binary, shell = '/bin/sh', uidArg, countArg = '30'] = process.argv.slice(2);
  const uid = uidArg === undefined ? process.getuid() : Number(uidArg);
  const count = Number(countArg);
  if (!binary || !Number.isInteger(uid) || uid < 0 ||
      !Number.isInteger(count) || count < 1 || count > 200)
    throw new Error('usage: node pty-tail-probe.js CODEX [SHELL [UID [COUNT]]]');
  const work = fs.mkdtempSync('/var/tmp/lx-pty-tail.');
  const home = path.join(work, 'home');
  fs.mkdirSync(home, { mode: 0o700 });
  fs.chownSync(work, uid, uid);
  fs.chownSync(home, uid, uid);
  const child = spawn(binary,
    ['app-server', '--stdio', '-c', 'sandbox_mode="danger-full-access"'], {
      cwd: work, detached: true, uid, gid: uid,
      env: { HOME: home, CODEX_HOME: home, PATH: '/usr/bin:/bin', SHELL: shell },
      stdio: ['pipe', 'pipe', 'pipe']
    });
  const pending = new Map(), output = new Map();
  const stream = readline.createInterface({ input: child.stdout });
  let nextId = 1, stderr = '', failed;
  child.stderr.on('data', data => { stderr = (stderr + data).slice(-2000); });
  child.on('error', error => { failed = error; });
  stream.on('line', line => {
    try {
      const message = JSON.parse(line);
      if (message.method === 'command/exec/outputDelta') {
        const p = message.params;
        output.set(p.processId, (output.get(p.processId) || '') +
          Buffer.from(p.deltaBase64, 'base64').toString('utf8'));
      }
      if (pending.has(message.id)) {
        const request = pending.get(message.id);
        pending.delete(message.id);
        clearTimeout(request.timer);
        if (message.error) request.reject(new Error(JSON.stringify(message.error)));
        else request.resolve(message.result);
      }
    } catch (error) { failed = error; }
  });
  function request(method, params) {
    return new Promise((resolve, reject) => {
      const id = nextId++;
      const timer = setTimeout(() => {
        pending.delete(id);
        reject(new Error('RPC timeout: ' + method));
      }, 8000);
      pending.set(id, { resolve, reject, timer });
      child.stdin.write(JSON.stringify({ id, method, params }) + '\n');
    });
  }
  async function ready(id, marker) {
    const deadline = Date.now() + 7000;
    while (!(output.get(id) || '').includes(marker)) {
      if (failed) throw failed;
      if (Date.now() >= deadline) throw new Error('PTY readiness timeout');
      await new Promise(resolve => setTimeout(resolve, 5));
    }
  }
  let missing = 0;
  try {
    await request('initialize', {
      clientInfo: { name: 'lx-pty-tail-probe', version: '1' },
      capabilities: { experimentalApi: true }
    });
    child.stdin.write(JSON.stringify({ method: 'initialized', params: {} }) + '\n');
    for (let i = 0; i < count; i++) {
      const processId = 'tail-' + i;
      const start = 'READY_' + i, tail = 'TAIL_OUTPUT_' + i;
      const done = request('command/exec', {
        command: [shell, '-c', `printf %s ${start}; IFS= read -r value; ` +
          `case "$value" in input) printf %s ${tail};; *) exit 9;; esac`],
        sandboxPolicy: { type: 'externalSandbox', networkAccess: 'enabled' },
        processId, tty: true, timeoutMs: 5000
      });
      // Handle a failed execution while waiting for its readiness notification.
      done.catch(error => { failed = error; });
      await ready(processId, start);
      await request('command/exec/write', {
        processId, deltaBase64: Buffer.from('input\n').toString('base64')
      });
      const result = await done;
      if (result.exitCode !== 0) throw new Error('PTY child exit: ' + result.exitCode);
      // Check after an additional command and a bounded notification drain.
      await request('command/exec', {
        command: [shell, '-c', 'printf BARRIER'],
        sandboxPolicy: { type: 'externalSandbox', networkAccess: 'enabled' },
        timeoutMs: 5000
      });
      await new Promise(resolve => setTimeout(resolve, 20));
      const captured = output.get(processId) || '';
      const found = captured.includes(tail);
      if (!found) missing++;
      console.log('PTY_TAIL_CASE=' + JSON.stringify({ i, uid, shell, found, captured }));
      output.delete(processId);
    }
    console.log('PTY_TAIL_RESULT=' + JSON.stringify({
      uid, shell, platform: os.platform(), count, missing, credentialsUsed: false
    }));
    process.exitCode = missing === 0 ? 0 : 1;
  } catch (error) {
    console.error(error.stack || error);
    console.error(stderr);
    process.exitCode = 2;
  } finally {
    for (const request of pending.values()) clearTimeout(request.timer);
    stream.close();
    child.stdin.destroy();
    try { process.kill(-child.pid, 'SIGTERM'); } catch (_) {}
    await new Promise(resolve => {
      if (child.exitCode !== null || child.signalCode !== null) return resolve();
      const timer = setTimeout(() => {
        try { process.kill(-child.pid, 'SIGKILL'); } catch (_) {}
        resolve();
      }, 3000);
      child.once('exit', () => { clearTimeout(timer); resolve(); });
    });
    fs.rmSync(work, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error.stack || error); process.exitCode = 2; });
