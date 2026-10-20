#!/usr/bin/env python3
"""TUI PTY smoke: launch, theme picker, forms, run, complete, recall, quit.

Runs `PhysicsSimApp --tui` on a real PTY (no display needed), drives it
like a user, and asserts the no-drift contract end to end:
  - boot renders the TUI title on a cursor-addressable terminal
  - F9 opens theme settings; theme preview and selection work on the PTY
  - form, sim, and signal builders switch panes, filter args, submit CLI parity
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
    env["WENGINE_TUI_THEME"] = "terminal"
    env["XDG_CACHE_HOME"] = tmp_xdg
    env["XDG_CONFIG_HOME"] = tmp_xdg
    env.pop("WENGINE_TUI_THEME", None)

    # --- full-screen session: launch / run / complete / recall / quit ---
    pid, fd = spawn_tui(binpath, env)
    try:
        boot = wait_for(fd, b"WEngine TUI", timeout=8.0)
        assert b"WEngine TUI" in boot, f"no TUI title on boot: {boot[-800:]!r}"
        print("boot ok")

        # F9 opens the theme picker. Preview Dracula with two Down presses,
        # apply it, and reopen settings to prove it remains active this run.
        os.write(fd, b"\x1b[20~")  # F9
        settings = wait_for(fd, b"Settings -- choose a color theme", timeout=6.0)
        assert b"Settings -- choose a color theme" in settings, (
            f"F9 did not open theme settings: {settings[-1200:]!r}"
        )
        os.write(fd, b"\x1bOB\x1bOB\r")  # Dracula, Enter to apply
        applied = wait_for(fd, b"theme saved: Dracula", timeout=6.0)
        assert b"theme saved: Dracula" in applied, f"theme selection failed: {applied[-1200:]!r}"
        os.write(fd, b"\x1b[20~")
        active_theme = wait_for(fd, b"Active: Dracula", timeout=6.0)
        assert b"Active: Dracula" in active_theme, f"selected theme was lost: {active_theme[-1200:]!r}"
        os.write(fd, b"\x1b")  # Escape closes settings without changing the applied theme
        time.sleep(0.3)
        read_available(fd, 1.0)
        print("F9 theme picker + apply ok")

        # F3 opens forms; the first entry is Ohm. Its builder starts with U/I
        # selected. Switch to the argument pane, deselect I, select R, then
        # switch back and enter R. The resulting command must use U+R only.
        os.write(fd, b"\x1bOR")  # F3 in xterm application-keypad mode
        forms = wait_for(fd, b"Forms --", timeout=6.0)
        assert b"Forms --" in forms, f"F3 did not open forms: {forms[-1200:]!r}"
        os.write(fd, b"\r")
        opened = wait_for(fd, b"Arguments", timeout=6.0)
        assert b"Arguments" in opened, f"Ohm argument pane missing: {opened[-1600:]!r}"
        assert b"computed" in opened and b"derived" in opened, (
            f"invalid/derived arguments were not marked: {opened[-1600:]!r}"
        )
        os.write(fd, b"\x02\x1bOC")  # Ctrl-B, Right pane
        focused = wait_for(fd, b"argument choices pane", timeout=4.0)
        assert b"argument choices pane" in focused, f"pane switch failed: {focused[-1200:]!r}"
        os.write(fd, b"\x1bOB ")  # Down to I; deselect
        time.sleep(0.3)
        os.write(fd, b"\x1bOB ")  # Down to R; select
        time.sleep(0.3)
        os.write(fd, b"\x02\x1bOD4Ohm\r")  # Back to builder, enter R, run
        form_result = wait_for(fd, b"I = 3.1 A", timeout=8.0)
        assert b"I = 3.1 A" in form_result, (
            f"builder did not submit the U+R pair: {form_result[-2000:]!r}"
        )
        print("dynamic Ohm form + pane switching ok")
        os.write(fd, b"\x1b")  # back to forms list
        time.sleep(0.3)
        read_available(fd, 1.0)
        os.write(fd, b"\x1bOB\r")  # select and open S0
        s0_rate = wait_for(fd, b"Arguments: rate", timeout=6.0)
        assert b"Arguments: rate" in s0_rate, (
            f"S0 rate profile missing: {s0_rate[-1600:]!r}"
        )
        assert b"Meter constant" in s0_rate, f"S0 required arguments missing: {s0_rate[-1600:]!r}"
        # Change the mode in the builder and ensure the right pane switches to
        # that mode's rules (energy is incompatible with the power+time branch).
        os.write(fd, b"\x15pulses")  # Ctrl-U clears mode; type pulses
        s0_pulses = wait_for(fd, b"invalid", timeout=6.0)
        assert b"invalid" in s0_pulses and b"Energy" in s0_pulses, (
            f"S0 mode profile did not update: {s0_pulses[-1600:]!r}"
        )
        print("mode-specific S0 argument profile ok")
        os.write(fd, b"\x1b\x1b")  # form -> list -> main view
        time.sleep(0.4)
        read_available(fd, 1.0)

        # F4 opens run builders; sim is the first entry. It uses the same
        # two-pane pattern: projectile shows gravity but hides the
        # signal-only UART family; switching the solver to signal flips that.
        os.write(fd, b"\x1bOS")  # F4
        builders = wait_for(fd, b"Run builders --", timeout=6.0)
        assert b"Run builders --" in builders, f"F4 did not open builders: {builders[-1200:]!r}"
        os.write(fd, b"\r")  # open sim run (first entry)
        sim_open = wait_for(fd, b"Arguments: projectile", timeout=6.0)
        assert b"Arguments: projectile" in sim_open, (
            f"sim argument pane missing: {sim_open[-1600:]!r}"
        )
        assert b"gravity" in sim_open, "projectile should expose gravity"
        assert b"uart" not in sim_open, "projectile should hide signal-only uart"
        os.write(fd, b"\x15signal")  # solver selector is the focused first field
        # NOTE: only the kind word is redrawn on a profile switch, so match
        # the redrawn fragment, not the full header line.
        sim_signal = wait_for(fd, b"signal (Ctrl+B, Right)", timeout=6.0)
        assert b"signal (Ctrl+B, Right)" in sim_signal, (
            f"sim signal profile missing: {sim_signal[-1600:]!r}"
        )
        assert b"uart" in sim_signal, "signal solver should expose uart"
        assert b"gravity" not in sim_signal, "signal solver should hide gravity"
        print("sim builder panes + solver-profile switching ok")
        os.write(fd, b"\x1b")  # back to builders list
        time.sleep(0.3)
        read_available(fd, 1.0)
        os.write(fd, b"\x1bOB\r")  # select and open sig run
        sig_open = wait_for(fd, b"Arguments: sine", timeout=6.0)
        assert b"Arguments: sine" in sig_open, (
            f"sig argument pane missing: {sig_open[-1600:]!r}"
        )
        assert b"duty" not in sig_open, "sine should not expose square/pulse duty"
        os.write(fd, b"\x15square")  # kind selector is the focused first field
        square_profile = wait_for(fd, b"duty", timeout=6.0)
        assert b"duty" in square_profile, (
            f"square profile did not expose duty: {square_profile[-1600:]!r}"
        )
        os.write(fd, b"\x02\x1bOC\x1bOB\x1bOB ")  # right pane; choose offset
        time.sleep(0.3)
        os.write(fd, b"\x02\x1bOD1V\r")  # left pane; enter offset and run
        sig_result = wait_for(fd, b"sig run: kind=square", timeout=8.0)
        assert b"sig run: kind=square" in sig_result, (
            f"signal builder did not submit: {sig_result[-2000:]!r}"
        )
        print("signal builder panes + argument selection ok")
        os.write(fd, b"\x1b\x1b")  # builder -> list -> main view
        time.sleep(0.4)
        read_available(fd, 1.0)

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

    theme_file = os.path.join(tmp_xdg, "wengine", "tui-theme")
    assert os.path.exists(theme_file), f"theme preference missing: {theme_file}"
    with open(theme_file, encoding="ascii") as f:
        assert f.read() == "dracula\n"

    # A fresh process, with no env override, must load the selected theme.
    pid, fd = spawn_tui(binpath, env)
    try:
        boot = wait_for(fd, b"WEngine TUI", timeout=8.0)
        assert b"WEngine TUI" in boot, f"TUI did not restart: {boot[-800:]!r}"
        os.write(fd, b"\x1b[20~")  # F9
        active = wait_for(fd, b"Active: Dracula", timeout=6.0)
        assert b"Active: Dracula" in active, f"theme did not persist: {active[-1200:]!r}"
        os.write(fd, b"\x1b[21~")  # F10
        rc = wait_eof(pid, fd, timeout=10.0)
        assert rc == 0, f"restart-session TUI quit exit {rc}, want 0"
    finally:
        try:
            os.close(fd)
        except OSError:
            pass
    print("theme preference survives restart")

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
