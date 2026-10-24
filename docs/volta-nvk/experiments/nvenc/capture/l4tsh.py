#!/usr/bin/env python3
"""l4tsh.py [--root] 'command': run a command on the board's L4T (user nx) over ssh.

ssh asks for nx's password (env L4T_PW, never stored here); --root runs the command
through sudo with the same password. stdin is not forwarded; output is streamed and
the exit status is the command's.
"""
import os
import pty
import sys

args = sys.argv[1:]
root = args and args[0] == "--root"
if root:
    args = args[1:]
cmd = args[0]
pw = os.environ["L4T_PW"]
marker = "__L4T_RC__"


def quote(s):
    return "'" + s.replace("'", "'\\''") + "'"


inner = f"export PAGER=cat; {cmd}; echo {marker}$?"
if root:
    inner = "sudo -S -p SUDOPW: bash -c " + quote(inner)
argv = ["ssh", "-tt", "-o", "UserKnownHostsFile=~/.ssh/known_hosts_l4t", "-o", "ConnectTimeout=10",
        "-o", "PreferredAuthentications=password,keyboard-interactive", "nx@192.168.1.234", inner]
pid, fd = pty.fork()
if pid == 0:
    os.execvp(argv[0], argv)

buf, sent, rc = b"", 0, 1
want = 2 if root else 1
while True:
    try:
        data = os.read(fd, 4096)
    except OSError:
        break
    if not data:
        break
    buf += data
    if sent < want and (b"assword" in buf or b"SUDOPW:" in buf):
        os.write(fd, (pw + "\n").encode())
        sent += 1
        buf = b""
        continue
    if sent:
        text = data.decode(errors="replace")
        if marker in text:
            head, _, tail = text.partition(marker)
            sys.stdout.write(head)
            digits = "".join(c for c in tail if c.isdigit())
            rc = int(digits) if digits else 1
        else:
            sys.stdout.write(text)
        sys.stdout.flush()
os.waitpid(pid, 0)
sys.exit(rc)
