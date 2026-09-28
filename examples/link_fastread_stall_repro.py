"""Cross-process reproducer for link_fastread end-of-run stalls (PHYSI-43).

Drives the built PhysicsSimApp publisher against link_fastread/link_sample
through the four end-of-run scenarios and traces the three sequence
positions that distinguish a real read failure from a reporting artifact:

  published   final frame_seq the publisher put on the bus
              (from `sim run`'s `shm: <name> (N frames)` line)
  cursor      highest frame_seq the reader observed
              (fastread `last_seq` status field)
  received    frames the reader actually consumed (--frames counts these,
              NOT published frames)

Scenarios (all bounded; exit 0 only when every expectation holds):

  reader_first   reader attached before a paced run, exact count:
                 completes with zero loss (control case).
  burst_exit     reader attached before an unpaced burst, want >> receivable:
                 the reader MUST still observe the published final sequence
                 (retained frames stay readable after publisher exit/unlink);
                 the idle timeout MUST print a final loss summary instead of
                 going silent.
  late_attach    reader started after a short flood already exited:
                 attach MUST fail fast-ish with `timed out waiting for
                 segment` (the segment lives only while the publisher lives;
                 no reader can attach afterwards by design).
  restart        publisher exits and republishes under the same name:
                 the stale reader MUST stay parked on the old run's final
                 frame (POSIX fd semantics: old mapping outlives the rerun,
                 undetectable by design); a freshly attached reader sees the
                 new run. Recovery is explicit reconnect (restart the reader),
                 mirroring gui::ShmSource::reconnect().

Usage:
  python3 examples/link_fastread_stall_repro.py [--build-dir build]

Needs a configured build dir with PhysicsSimApp, link_fastread and
link_sample (cmake -S . -B build && cmake --build build).
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time

PUBLISHED_RE = re.compile(r"shm: \S+ \((\d+) frames\)")
LAST_SEQ_RE = re.compile(r"last_seq=(\d+)")
RECEIVED_RE = re.compile(r"received=(\d+)")


def run_pub(sim: str, seg: str, steps: int, extra: list[str]) -> tuple[int, int]:
    """Run a headless publisher burst; return (exit code, published frames)."""
    proc = subprocess.run(
        [sim, "sim", "run", "--steps", str(steps), "--particles", "1",
         "--emit-shm", seg, *extra],
        capture_output=True, text=True, timeout=60)
    published = 0
    match = PUBLISHED_RE.search(proc.stdout + proc.stderr)
    if match:
        published = int(match.group(1))
    return proc.returncode, published


def start_reader(binpath: str, seg: str, args: list[str]):
    """Spawn a reader tool; start order vs the publisher is the test."""
    return subprocess.Popen(
        [binpath, seg, *args],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def finish(proc: subprocess.Popen, timeout: float):
    try:
        out, err = proc.communicate(timeout=timeout)
        return proc.returncode, out, err, False
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
        return proc.returncode, out, err, True


def reader_first(build: str) -> bool:
    """Control: paced run, exact receive count, zero loss expected."""
    seg = "/wengine-repro-reader-first"
    sim = os.path.join(build, "PhysicsSimApp")
    fast = os.path.join(build, "link_fastread")
    reader = start_reader(
        fast, seg, ["--frames", "10", "--timeout-ms", "10000",
                    "--status-every", "0"])
    time.sleep(0.3)
    rc, published = run_pub(sim, seg, 10, ["--paced", "--rate", "50Hz"])
    code, out, err, stalled = finish(reader, 15.0)
    ok = (not stalled and code == 0 and published == 10
          and "received=10 missed=0" in out)
    print(f"[reader_first] pub_rc={rc} published={published} "
          f"reader_rc={code} stalled={stalled} ok={ok}")
    print(f"  tail: {out.strip().splitlines()[-1] if out.strip() else ''}"
          f" err: {err.strip()[-120:]}")
    return ok


def burst_exit(build: str) -> bool:
    """Attached reader must see the published final seq after burst + exit,
    and the unreachable receive-count target must end in a loss summary."""
    seg = "/wengine-repro-burst-exit"
    sim = os.path.join(build, "PhysicsSimApp")
    fast = os.path.join(build, "link_fastread")
    # status-every 0: the only stdout on timeout must be the final summary.
    reader = start_reader(
        fast, seg, ["--frames", "100000", "--timeout-ms", "4000",
                    "--status-every", "0"])
    time.sleep(0.3)
    rc, published = run_pub(sim, seg, 200, [])
    code, out, err, stalled = finish(reader, 12.0)
    cursors = [int(v) for v in LAST_SEQ_RE.findall(out)]
    cursor = cursors[-1] if cursors else 0
    # Final frame observed (no read blindness) + bounded idle-timeout exit
    # with a loss summary (not silence): received/missed/last_seq present.
    summary = ("missed=" in out and f"last_seq={published}" in out)
    ok = (rc == 0 and published == 200 and not stalled and code == 1
          and cursor == published and summary)
    print(f"[burst_exit] pub_rc={rc} published={published} cursor={cursor} "
          f"reader_rc={code} stalled={stalled} summary={summary} ok={ok}")
    lines = out.strip().splitlines()
    for line in lines[:1] + lines[-1:]:
        print(f"  out: {line}")
    print(f"  err: {err.strip()[-160:]}")
    return ok


def late_attach(build: str) -> bool:
    """Reader started after a short flood exited: attach must time out with
    the segment message (never a hang, never phantom frames)."""
    seg = "/wengine-repro-late-attach"
    sim = os.path.join(build, "PhysicsSimApp")
    fast = os.path.join(build, "link_fastread")
    rc, published = run_pub(sim, seg, 5, [])
    reader = start_reader(fast, seg, ["--frames", "5", "--timeout-ms", "2000",
                                      "--status-every", "0"])
    code, out, err, stalled = finish(reader, 8.0)
    ok = (rc == 0 and published == 5 and not stalled and code == 1
          and "timed out waiting for segment" in err and out.strip() == "")
    print(f"[late_attach] pub_rc={rc} published={published} reader_rc={code} "
          f"stalled={stalled} ok={ok}")
    print(f"  err: {err.strip()[-160:]}")
    return ok


def restart(build: str) -> bool:
    """Same-name republish: the reader attached to run 1 stays parked on its
    final frame through run 2 (stale mapping, undetectable by design); a
    reader attached during run 2 sees fresh frames. Recovery is explicit
    reconnect (restart the reader), mirroring gui::ShmSource::reconnect()."""
    seg = "/wengine-repro-restart"
    sim = os.path.join(build, "PhysicsSimApp")
    fast = os.path.join(build, "link_fastread")
    stale = start_reader(
        fast, seg, ["--frames", "0", "--timeout-ms", "2500",
                    "--status-every", "1"])
    time.sleep(0.2)
    rc1, pub1 = run_pub(sim, seg, 5, ["--paced", "--rate", "20Hz"])
    # Fresh reader attached during run 2: must complete on new-run frames.
    fresh = start_reader(
        fast, seg, ["--frames", "2", "--timeout-ms", "5000",
                    "--status-every", "0"])
    rc2, pub2 = run_pub(sim, seg, 9, ["--paced", "--rate", "20Hz"])
    fresh_code, fresh_out, fresh_err, fresh_stalled = finish(fresh, 10.0)
    stale_code, stale_out, stale_err, stale_stalled = finish(stale, 10.0)
    stale_cursors = [int(v) for v in LAST_SEQ_RE.findall(stale_out)]
    stale_max = max(stale_cursors) if stale_cursors else 0
    fresh_done = (not fresh_stalled and fresh_code == 0
                  and "received=2 missed=0" in fresh_out)
    # Stale reader never advances past run 1's final frame even though run 2
    # published 9 more under the same name; it idle-times out with a summary.
    stale_parked = (not stale_stalled and stale_code == 1 and stale_max == 5
                    and "last_seq=5" in stale_out)
    ok = (rc1 == 0 and rc2 == 0 and pub1 == 5 and pub2 == 9
          and fresh_done and stale_parked)
    print(f"[restart] run1={pub1} run2={pub2} fresh_rc={fresh_code} "
          f"stale_max={stale_max} stale_rc={stale_code} ok={ok}")
    print(f"  fresh: {fresh_out.strip()[-160:]} {fresh_err.strip()[-80:]}")
    stale_lines = stale_out.strip().splitlines()
    if stale_lines:
        print(f"  stale last: {stale_lines[-1]}")
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", default="build")
    args = parser.parse_args()
    for tool in ("PhysicsSimApp", "link_fastread", "link_sample"):
        if not os.path.isfile(os.path.join(args.build_dir, tool)):
            print(f"missing {args.build_dir}/{tool}: configure and build first",
                  file=sys.stderr)
            return 2
    results = [
        ("reader_first", reader_first(args.build_dir)),
        ("burst_exit", burst_exit(args.build_dir)),
        ("late_attach", late_attach(args.build_dir)),
        ("restart", restart(args.build_dir)),
    ]
    failed = [name for name, ok in results if not ok]
    print(f"{len(results) - len(failed)}/{len(results)} scenarios as expected"
          + (f"; FAILED: {', '.join(failed)}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
