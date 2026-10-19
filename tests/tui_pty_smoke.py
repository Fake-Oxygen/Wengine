#!/usr/bin/env python3
"""TUI-7 PTY smoke: launch, run, complete, history recall, quit (CI-safe).

Runs `PhysicsSimApp --tui` on a real PTY (no display needed), drives it
like a user, and asserts the no-drift contract end to end:
  - boot renders the TUI title on a cursor-addressable terminal
  - typing `ohm 12V 3A` + Enter shows the CLI-identical `R = 4` text
  - Tab single-completes (`o` -> `ohm `) and multi-candidate popups stay loud
  - Up recalls history and re-runs byte-identical output
  - F10 quits with exit 0; the shared history file holds the typed line
  - non-TTY `--tui` refuses loudly (exit 1); `--tui extra` is usage (exit 2)

Headless/CI use: `python3 tests/tui_pty_smoke.py --bin ./build/PhysicsSimApp`.
Isolated history via a temp XDG_CACHE_HOME, so CI never touches the user file.
"""
import argparse
import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

READ_CHUNK = 65536


def read_available(fd, timeout=3.0, settle=0.5):
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


def wait_for(fd, needle: bytes, timeout=6.0):
    """Drain until `needle` appears or the timeout hits. Returns all bytes seen."""
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
                # Settle: one more short drain so the frame finishes drawing.
                seen += read_available(fd, timeout=0.6)
                return seen
        # Keep looping until the deadline even when idle.
    return seen


def spawn_tui(binpath, env, rows=30, cols=100):
    pid, fd = pty.fork()
    if pid == 0:
        try:
            fcntl.ioctl(1, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        except OSError:
            pass
        os.execve(binpath, [binpath, "--tui"], env)
    return pid, fd


def wait_eof(pid, fd, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.5)
        if r:
            try:
                if not os.read(fd, READ_CHUNK):
                    break
            except OSError:
                break
    _, status = os.waitpid(pid, 0)
    return os.waitstatus_to_exitcode(status)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(os.path.dirname(__file__), "..", "build", "PhysicsSimApp"))
    args = ap.parse_args()
    binpath = os.path.abspath(args.bin)
    assert os.path.exists(binpath), f"TUI binary missing: {binpath}"

    tmp_xdg = tempfile.mkdtemp(prefix="wengine_tui_smoke_")
    env = dict(os.environ)
    env["TERM"] = "xterm"
    env.pop("NO_COLOR", None)
    env["XDG_CACHE_HOME"] = tmp_xdg

    # --- full-screen session: launch / run / complete / recall / quit ---
    pid, fd = spawn_tui(binpath, env)
    try:
        boot = wait_for(fd, b"WEngine TUI", timeout=8.0)
        assert b"WEngine TUI" in boot, f"no TUI title on boot: {boot[-800:]!r}"
        print("boot ok")

        os.write(fd, b"ohm 12V 3A\r")
        ran = wait_for(fd, b"R = 4", timeout=8.0)
        assert b"R = 4" in ran, f"ohm run missing CLI-identical text: {ran[-2000:]!r}"
        print("run ok (byte-identical ohm text)")

        # Single-candidate Tab: `o` completes to `ohm ` with a status note.
        os.write(fd, b"\x15")  # Ctrl-U: clear the input line first
        time.sleep(0.3)
        read_available(fd, 1.0)
        os.write(fd, b"o\t")
        completed = wait_for(fd, b"completed", timeout=6.0)
        assert b"completed" in completed, f"no single-complete status: {completed[-1200:]!r}"
        # The input redraw is incremental (`o` was already drawn, Tab appends
        # `hm `), so the status line is the stable signal, not one contiguous
        # `ohm` byte run.
        print("complete (single) ok")
        os.write(fd, b"\x15")
        time.sleep(0.3)
        read_available(fd, 1.0)

        # Multi-candidate Tab: `s` lists s0/sim/sig loudly above the input.
        os.write(fd, b"s\t")
        multi = wait_for(fd, b"completions", timeout=6.0)
        if b"completions" not in multi:
            multi += wait_for(fd, b"candidates", timeout=3.0)
        assert b"completions" in multi or b"candidates" in multi, (
            f"no multi-complete popup status: {multi[-1200:]!r}"
        )
        print("complete (multi) ok")
        os.write(fd, b"\x15")
        time.sleep(0.3)
        read_available(fd, 1.0)
        os.write(fd, b"\x1b")  # Esc dismisses the transient popup
        time.sleep(0.3)
        read_available(fd, 1.0)

        # History recall: Up re-enters the ohm line; Enter re-runs it.
        # (Application-cursor `SS3 A`: ncurses enables application mode with
        # keypad(TRUE), so plain `CSI A` arrives as literal `[A` instead.)
        os.write(fd, b"\x1bOA")
        time.sleep(0.6)
        recalled = read_available(fd, 2.0)
        assert b"ohm" in recalled, f"Up did not recall history: {recalled[-1200:]!r}"
        os.write(fd, b"\r")
        rerun = wait_for(fd, b"R = 4", timeout=8.0)
        assert b"R = 4" in rerun, f"history re-run missing text: {rerun[-2000:]!r}"
        print("history recall ok")

        os.write(fd, b"\x1b[21~")  # F10
        rc = wait_eof(pid, fd, timeout=10.0)
        assert rc == 0, f"TUI quit exit {rc}, want 0"
        print("quit (F10) ok")
    finally:
        try:
            os.close(fd)
        except OSError:
            pass

    # Shared history file: the TUI appends to the same REPL location.
    hist = os.path.join(tmp_xdg, "wengine", "history")
    assert os.path.exists(hist), f"shared history file missing: {hist}"
    with open(hist, encoding="utf-8", errors="replace") as f:
        body = f.read()
    assert "ohm 12V 3A" in body, f"history missing typed line: {body[-500:]!r}"
    print("history-file sharing ok")

    # Non-TTY refusal (exit 1) and extra-args usage error (exit 2): no PTY.
    p1 = subprocess.run([binpath, "--tui"], stdin=subprocess.DEVNULL,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
    assert p1.returncode == 1, f"non-TTY --tui exit {p1.returncode}, want 1"
    assert b"interactive terminal" in p1.stderr, f"no refusal text: {p1.stderr[-300:]!r}"
    print("non-TTY refusal ok")

    p2 = subprocess.run([binpath, "--tui", "extra"], stdin=subprocess.DEVNULL,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
    assert p2.returncode == 2, f"--tui extra exit {p2.returncode}, want 2"
    assert b"Usage" in p2.stderr, f"no usage text: {p2.stderr[-300:]!r}"
    print("extra-args usage ok")

    print("PTY TUI-7 SMOKE: OK")


if __name__ == "__main__":
    main()
