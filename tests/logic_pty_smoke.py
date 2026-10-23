#!/usr/bin/env python3
"""Logic session PTY smoke: prompt swap, scoped completion, back.

Runs `PhysicsSimApp` (REPL) on a real PTY — raw-mode line editing only
engages on a TTY, so pipes cannot prove this — and asserts the LOGIC-1
contract end to end:
  - `log` + Tab completes to `logic ` (top-level vocabulary)
  - bare `logic` + Enter swaps the prompt to `[logic]> `
  - `b` + Tab completes to `back ` (module-scoped vocabulary only)
  - `help` lists the session verbs (`help`, `back`)
  - `back` leaves the session (`Leaving logic session.` + `> ` prompt)
  - `back` outside the session is a loud unknown command (rc 1 shape)
  - `exit` quits with exit 0

Headless/CI use: `python3 tests/logic_pty_smoke.py --bin ./build/PhysicsSimApp`.
Isolated history via a temp XDG_CACHE_HOME, so CI never touches the user file.
"""

import argparse
import os
import pty
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time
import fcntl

READ_CHUNK = 65536


def read_available(fd, timeout=3.0, settle=0.4):
    out = b""
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                chunk = os.read(fd, READ_CHUNK)
            except OSError:
                break
            if not chunk:
                break
            out += chunk
            end = time.time() + settle
        elif out:
            break
    return out


def wait_for(fd, needle: bytes, timeout=8.0):
    seen = b""
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                chunk = os.read(fd, READ_CHUNK)
            except OSError:
                break
            if not chunk:
                break
            seen += chunk
            if needle in seen:
                seen += read_available(fd, timeout=0.6)
                return seen
    return seen


def fail(msg, seen):
    sys.stderr.write(f"FAIL: {msg}\n--- bytes seen ---\n{seen!r}\n")
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="wengine-logic-pty-")
    env = dict(os.environ, XDG_CACHE_HOME=tmp, TERM="xterm")

    pid, fd = pty.fork()
    if pid == 0:
        try:
            fcntl.ioctl(1, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
        except OSError:
            pass
        os.execve(args.bin, [args.bin], env)
        os._exit(127)

    def send(text: str):
        os.write(fd, text.encode())

    rc = 0
    try:
        seen = wait_for(fd, b"> ")
        if b"> " not in seen:
            return fail("no top-level prompt", seen)

        # Top-level completion: `log` -> `logic ` (then Enter).
        send("log\t")
        seen = wait_for(fd, b"logic ")
        if b"logic " not in seen:
            return fail("top-level Tab did not complete to 'logic '", seen)
        send("\n")
        seen = wait_for(fd, b"[logic]> ")
        if b"[logic]> " not in seen:
            return fail("no [logic]> prompt after bare `logic`", seen)
        if b"Entering logic session" not in seen:
            return fail("no session entry banner", seen)

        # Session help lists the module verbs.
        send("help\n")
        seen = wait_for(fd, b"[logic]> ")
        if b"Logic session commands" not in seen or b"back - Leave" not in seen:
            return fail("session `help` did not list module verbs", seen)

        # Scoped completion: `b` -> `back ` (then Enter leaves).
        send("b\t")
        seen = wait_for(fd, b"back ")
        if b"back " not in seen:
            return fail("session Tab did not complete to 'back '", seen)
        send("\n")
        seen = wait_for(fd, b"> ")
        if b"Leaving logic session." not in seen:
            return fail("no session leave banner after `back`", seen)

        # `back` outside the session is a loud unknown command.
        send("back\n")
        seen = wait_for(fd, b"> ")
        if b"Unknown command: back" not in seen:
            return fail("top-level `back` was not a loud unknown command", seen)

        send("exit\n")
        _, status = os.waitpid(pid, 0)
        pid = -1
        if not (os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0):
            return fail(f"exit status was {status}", b"")
    finally:
        if pid != -1:
            try:
                os.kill(pid, 9)
            except OSError:
                pass
            try:
                os.waitpid(pid, 0)
            except OSError:
                pass
        try:
            os.close(fd)
        except OSError:
            pass

    print("logic_pty_smoke: OK")
    return rc


if __name__ == "__main__":
    sys.exit(main())
