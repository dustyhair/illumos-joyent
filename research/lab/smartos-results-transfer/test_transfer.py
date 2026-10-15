import base64
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import transfer as t


class TransferTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        # A parser fixture, not a generated cryptographic key.
        self.pub = self.root / 'fixture.pub'
        blob = b'\x00\x00\x00\x0bssh-ed25519\x00\x00\x00\x20' + bytes(32)
        self.pub.write_text('ssh-ed25519 ' + base64.b64encode(blob).decode() + ' fixture\n')

    def test_network_loopback_restricted(self):
        self.assertIn('restrict=on,hostfwd=tcp:127.0.0.1:22222-10.0.2.15:2222', t.QEMU_ARGS[1])

    def make_bundle(self, data=b'example result\n'):
        source = '/var/tmp/prelaunch-' + 'a' * 32 + '/verified.jsonl'
        out = self.root / 'bundle'
        t.bundle(out, self.pub, source, len(data), hashlib.sha256(data).hexdigest())
        return out, source

    def test_bundle_forces_one_reader_and_no_password_or_forwarding(self):
        out, source = self.make_bundle()
        config = (out / 'sshd_config').read_text()
        for text in ['Port 2222', 'ListenAddress 10.0.2.15', 'PasswordAuthentication no',
                     'ChallengeResponseAuthentication no', 'PermitRootLogin forced-commands-only',
                     'AllowTcpForwarding no', 'AllowStreamLocalForwarding no', 'AllowAgentForwarding no',
                     'AuthenticationMethods publickey', 'KbdInteractiveAuthentication no',
                     'PermitTTY no', 'PermitUserRC no', 'DisableForwarding yes']:
            self.assertIn(text, config)
        self.assertNotIn('UsePAM no', config)
        key = (out / 'authorized_keys').read_text()
        self.assertIn('command="/bin/bash /root/codex-result-ssh/read-result.sh"', key)
        self.assertIn('no-pty', key)
        self.assertIn(source, (out / 'read-result.sh').read_text())
        self.assertFalse((out / 'host_ed25519').exists())
        with self.assertRaises(FileExistsError):
            self.make_bundle()

    def test_reader_success_denies_other_command_and_changed_file(self):
        data = b'example result\n'
        out, source = self.make_bundle(data)
        local = self.root / 'verified.jsonl'
        local.write_bytes(data)
        script = (out / 'read-result.sh').read_text().replace(source, str(local))
        script = script.replace('/usr/bin/digest', 'digest')
        shim = 'digest() { /usr/bin/sha256sum "$3" | /usr/bin/awk \'{print $1}\'; };\n'
        for command, expected in [('read-result', 0), ('sh', 1), ('', 1)]:
            result = subprocess.run(['/bin/bash', '-c', shim + script],
                                    env=dict(os.environ, SSH_ORIGINAL_COMMAND=command), capture_output=True)
            self.assertEqual(result.returncode, expected)
            self.assertEqual(result.stdout, data if expected == 0 else b'')
        local.write_bytes(b'wrong contents')
        result = subprocess.run(['/bin/bash', '-c', shim + script],
                                env=dict(os.environ, SSH_ORIGINAL_COMMAND='read-result'), capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b'')

    def test_paths_and_public_key_options_rejected(self):
        with self.assertRaises(ValueError):
            t.bundle(self.root / 'bad', self.pub, '/etc/shadow', 1, 'a' * 64)
        self.pub.write_text('command="sh" ' + self.pub.read_text())
        with self.assertRaises(ValueError):
            t.public_key(self.pub)

    def test_host_pin_is_exclusive(self):
        path = self.root / 'known_hosts'
        t.pin_host(self.pub, path)
        self.assertTrue(path.read_text().startswith('[127.0.0.1]:22222 ssh-ed25519 '))
        with self.assertRaises(FileExistsError):
            t.pin_host(self.pub, path)

    def test_ssh_policy(self):
        known = self.root / 'known_hosts'
        t.pin_host(self.pub, known)
        command = t.ssh_command(self.pub, known)
        for value in ['StrictHostKeyChecking=yes', 'GlobalKnownHostsFile=/dev/null',
                      'BatchMode=yes', 'IdentityAgent=none', 'ClearAllForwardings=yes']:
            self.assertIn(value, command)
        self.assertEqual(command[-2:], ['root@127.0.0.1', 'read-result'])

    def receive(self, program, expected=b'payload', **kwargs):
        return t.receive([sys.executable, '-c', program], self.root / 'result',
                         len(expected), hashlib.sha256(expected).hexdigest(), **kwargs)

    def test_receive_exact_hash_exit_and_no_overwrite(self):
        result = self.receive("import sys;sys.stdout.buffer.write(b'payload')")
        self.assertEqual(result['bytes'], 7)
        self.assertEqual((self.root / 'result').read_bytes(), b'payload')
        with self.assertRaises(FileExistsError):
            self.receive("raise Exception('must not execute')")

    def test_wrong_hash_short_oversize_and_nonzero_exit_keep_partial(self):
        for program in ["print('wrong!',end='')", "print('x',end='')",
                        "print('payloadextra',end='')", "print('payload',end='');exit(7)"]:
            with self.subTest(program=program):
                with self.assertRaises((ValueError, RuntimeError)):
                    self.receive(program)
                self.assertFalse((self.root / 'result').exists())
                self.assertLessEqual((self.root / 'result.partial').stat().st_size, 7)
                (self.root / 'result.partial').unlink()

    def test_timeout_keeps_partial_and_does_not_publish(self):
        with self.assertRaises(TimeoutError):
            self.receive('import time;time.sleep(10)', timeout=0.1)
        self.assertFalse((self.root / 'result').exists())
        self.assertTrue((self.root / 'result.partial').exists())

    def test_symlink_partial_rejected(self):
        target = self.root / 'untouched'
        target.write_text('keep')
        (self.root / 'result.partial').symlink_to(target)
        with self.assertRaises(FileExistsError):
            self.receive("print('payload',end='')")
        self.assertEqual(target.read_text(), 'keep')


if __name__ == '__main__':
    unittest.main()
