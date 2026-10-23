#!/usr/bin/env python3
"""jroot.py 'command': run a shell command as root on nx-linux via `su` over ssh.

The root password is read from the JROOT_PW environment variable (never stored here).
Output is streamed; the exit status is the command's.
"""
import os
import pty
import sys

cmd = sys.argv[1]
pw = os.environ["JROOT_PW"]
marker = "__JROOT_RC__"


def quote(s):
    return "'" + s.replace("'", "'\\''") + "'"


inner = f"export PAGER=cat; {cmd}; echo {marker}$?"
argv = ["ssh", "-tt", "nx-linux", "su - root -c " + quote(inner)]
pid, fd = pty.fork()
if pid == 0:
    os.execvp(argv[0], argv)

buf, sent, rc = b"", False, 1
while True:
    try:
        data = os.read(fd, 4096)
    except OSError:
        break
    if not data:
        break
    buf += data
    if not sent and b"assword" in buf:
        os.write(fd, (pw + "\n").encode())
        sent, buf = True, b""
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
