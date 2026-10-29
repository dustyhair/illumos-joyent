#!/usr/bin/env python3
"""Send a command to this thread's private QEMU serial console."""
import pathlib
import base64
import re
import socket
import sys
import time

lab = pathlib.Path(__file__).resolve().parent / "lab"
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
    client.settimeout(0.5)
    client.connect(str(lab / "serial.sock"))
    if len(sys.argv) == 4 and sys.argv[1] == "--upload":
        target = sys.argv[3]
        if not re.fullmatch(r"/lab/[a-zA-Z0-9_.-]+", target):
            raise SystemExit("Upload target must be a file directly under /lab")
        data = pathlib.Path(sys.argv[2]).read_bytes()
        if len(data) > 32768:
            raise SystemExit("Serial uploads are limited to 32 KiB")
        payload = base64.encodebytes(data).decode()
        command = (
            f"/usr/bin/openssl enc -d -base64 > {target} <<'LX_UPLOAD_EOF'\n"
            + payload + "LX_UPLOAD_EOF\n"
            + f"chmod 0555 {target}\n"
        )
        for line in command.splitlines():
            client.sendall((line + "\r").encode())
            time.sleep(0.02)
    elif len(sys.argv) > 1:
        client.sendall((sys.argv[1] + "\r").encode())
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        try:
            data = client.recv(16384)
        except socket.timeout:
            break
        if not data:
            break
        sys.stdout.write(data.decode(errors="replace"))
