#!/usr/bin/env python3
"""Offline SSH bundle preparation and an explicit bounded, pinned host pull.

Never generates keys, changes QEMU, or starts a daemon.
"""
import argparse
import base64
import hashlib
import os
from pathlib import Path
import re
import selectors
import subprocess
import time

GUEST_DIR = '/root/codex-result-ssh'
QEMU_ARGS = ['-netdev', 'user,id=labssh,restrict=on,hostfwd=tcp:127.0.0.1:22222-10.0.2.15:2222',
             '-device', 'e1000,netdev=labssh,id=labsshdev']
CAP = 128 * 1024 * 1024


def public_key(path):
    fields = Path(path).read_text().strip().split()
    if len(fields) < 2 or fields[0] != 'ssh-ed25519':
        raise ValueError('Expected a dedicated Ed25519 public key, without authorized-key options')
    blob = base64.b64decode(fields[1], validate=True)
    if len(blob) != 51 or blob[:19] != b'\x00\x00\x00\x0bssh-ed25519\x00\x00\x00\x20':
        raise ValueError('Malformed Ed25519 public key')
    return fields[0] + ' ' + fields[1]


def validate_result(source, size, digest):
    if not re.fullmatch(r'/var/tmp/prelaunch-[0-9a-f]{32}/verified\.jsonl', source):
        raise ValueError('Bind exactly one /var/tmp/prelaunch-<run>/verified.jsonl')
    if not 0 < size <= CAP or not re.fullmatch('[0-9a-f]{64}', digest):
        raise ValueError('Invalid expected size or SHA256')


def write_new(path, text):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())


def bundle(output, client_public_key, source, size, digest):
    validate_result(source, size, digest)
    key = public_key(client_public_key)
    output = Path(output)
    output.mkdir(mode=0o700)  # Never replace an existing bundle.
    reader = f'''#!/bin/bash
set -e
test "${{SSH_ORIGINAL_COMMAND-}}" = read-result
source='{source}'
test -d "${{source%/*}}" && test ! -L "${{source%/*}}"
test -f "$source" && test ! -L "$source"
test "$(wc -c < "$source")" -eq {size}
test "$(/usr/bin/digest -a sha256 "$source")" = {digest}
/usr/bin/cat "$source"
test "$(wc -c < "$source")" -eq {size}
test "$(/usr/bin/digest -a sha256 "$source")" = {digest}
'''
    config = f'''Port 2222
ListenAddress 10.0.2.15
HostKey {GUEST_DIR}/host_ed25519
PidFile {GUEST_DIR}/sshd.pid
AuthorizedKeysFile {GUEST_DIR}/authorized_keys
AllowUsers root
StrictModes yes
PubkeyAuthentication yes
PasswordAuthentication no
ChallengeResponseAuthentication no
PermitEmptyPasswords no
PermitRootLogin forced-commands-only
AllowTcpForwarding no
AllowStreamLocalForwarding no
AllowAgentForwarding no
X11Forwarding no
PermitTunnel no
PermitUserEnvironment no
UseDNS no
AuthenticationMethods publickey
KbdInteractiveAuthentication no
PermitTTY no
PermitUserRC no
DisableForwarding yes
LogLevel VERBOSE
'''
    options = f'command="/bin/bash {GUEST_DIR}/read-result.sh",no-port-forwarding,no-agent-forwarding,no-X11-forwarding,no-pty'
    write_new(output / 'read-result.sh', reader)
    write_new(output / 'sshd_config', config)
    write_new(output / 'authorized_keys', options + ' ' + key + '\n')
    write_new(output / 'qemu-network-args.txt', '\n'.join(QEMU_ARGS) + '\n')
    write_new(output / 'expected.txt', f'{source}\n{size}\n{digest}\n')


def pin_host(trusted_console_public_key, destination):
    # Caller must obtain this PUBLIC key via the trusted guest console, not keyscan.
    write_new(destination, '[127.0.0.1]:22222 ' + public_key(trusted_console_public_key) + '\n')


def ssh_command(key, known_hosts):
    for path in (key, known_hosts):
        if not Path(path).is_file():
            raise ValueError('Key and console-pinned known_hosts must already exist')
    return ['ssh', '-F', '/dev/null', '-T', '-p', '22222', '-i', str(Path(key).resolve()),
            '-o', 'IdentitiesOnly=yes', '-o', 'IdentityAgent=none',
            '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes',
            '-o', 'UserKnownHostsFile=' + str(Path(known_hosts).resolve()),
            '-o', 'GlobalKnownHostsFile=/dev/null', '-o', 'UpdateHostKeys=no',
            '-o', 'PasswordAuthentication=no', '-o', 'KbdInteractiveAuthentication=no',
            '-o', 'PreferredAuthentications=publickey', '-o', 'ClearAllForwardings=yes',
            '-o', 'ConnectTimeout=10', '-o', 'ServerAliveInterval=10',
            '-o', 'ServerAliveCountMax=3', 'root@127.0.0.1', 'read-result']


def receive(command, destination, size, digest, timeout=180):
    """Exclusive partial output; exact length/hash/exit required for promotion.

    On failure retain the partial; never retry. Only the local SSH client is killed.
    """
    if not 0 < size <= CAP or not re.fullmatch('[0-9a-f]{64}', digest) or not 0 < timeout <= 7200:
        raise ValueError('Invalid size/hash/timeout')
    destination = Path(destination)
    partial = destination.with_name(destination.name + '.partial')
    if destination.exists() or destination.is_symlink():
        raise FileExistsError(destination)
    fd = os.open(partial, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    total = 0
    hasher = hashlib.sha256()
    stderr = bytearray()
    process = None
    try:
        with os.fdopen(fd, 'wb') as output:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            deadline = time.monotonic() + timeout
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ, 'out')
                selector.register(process.stderr, selectors.EVENT_READ, 'err')
                while selector.get_map():
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError('Transfer timed out; partial retained, no retry')
                    for event, _ in selector.select(min(1, remaining)):
                        data = os.read(event.fileobj.fileno(), 65536)
                        if not data:
                            selector.unregister(event.fileobj)
                            continue
                        if event.data == 'err':
                            stderr.extend(data)
                            if len(stderr) > 65536:
                                raise ValueError('SSH stderr exceeds bound')
                        else:
                            if total + len(data) > size:
                                raise ValueError('Transfer exceeds expected length')
                            output.write(data)
                            hasher.update(data)
                            total += len(data)
            rc = process.wait(timeout=max(0.001, deadline - time.monotonic()))
            if rc != 0:
                raise RuntimeError(f'SSH exited {rc}: {stderr.decode(errors="replace")}')
            if total != size or hasher.hexdigest() != digest:
                raise ValueError('Transferred size or SHA256 mismatch')
            output.flush()
            os.fsync(output.fileno())
        # Atomic no-overwrite publication on this same filesystem.
        os.link(partial, destination, follow_symlinks=False)
        partial.unlink()
        directory = os.open(destination.parent, os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
        return {'bytes': total, 'sha256': hasher.hexdigest(), 'ssh_exit': rc}
    finally:
        if process is not None:
            if process.poll() is None:
                process.kill()
            process.wait()
            process.stdout.close()
            process.stderr.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    p = sub.add_parser('bundle')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--client-public-key', type=Path, required=True)
    p.add_argument('--source', required=True)
    p.add_argument('--size', type=int, required=True)
    p.add_argument('--sha256', required=True)
    p = sub.add_parser('pin-host')
    p.add_argument('--trusted-console-public-key', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p = sub.add_parser('pull')
    p.add_argument('--key', type=Path, required=True)
    p.add_argument('--known-hosts', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--size', type=int, required=True)
    p.add_argument('--sha256', required=True)
    p.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    if args.action == 'bundle':
        bundle(args.output, args.client_public_key, args.source, args.size, args.sha256)
    elif args.action == 'pin-host':
        pin_host(args.trusted_console_public_key, args.output)
    else:
        print(receive(ssh_command(args.key, args.known_hosts), args.output,
                      args.size, args.sha256, args.timeout))


if __name__ == '__main__':
    main()
