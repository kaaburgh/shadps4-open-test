#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import selectors
import subprocess
import sys
import time

MARKER = re.compile(r"SHADTEST\s+name=(?P<name>\S+)\s+status=(?P<status>PASS|FAIL)\b(?P<detail>.*)")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("test", nargs="?", default="gpu_solid_rt")
    parser.add_argument("--shadps4", default=os.environ.get("SHADPS4"))
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument(
        "--allow-no-display",
        action="store_true",
        help="try anyway; current upstream shadPS4 normally needs SDL video",
    )
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    elf = root / "out" / args.test / f"{args.test}.elf"
    log_path = root / "out" / args.test / "run.log"

    if not args.shadps4:
        print("SHADPS4 is not set and --shadps4 was not supplied", file=sys.stderr)
        return 2
    if not elf.is_file():
        print(f"missing test ELF: {elf}; run ./scripts/build-test.sh {args.test}", file=sys.stderr)
        return 2
    if (
        sys.platform.startswith("linux")
        and not args.allow_no_display
        and not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"))
    ):
        print(
            "no DISPLAY/WAYLAND_DISPLAY; current upstream shadPS4 has no usable CLI "
            "headless mode and SDL video init is expected to fail",
            file=sys.stderr,
        )
        return 2

    command = [args.shadps4, str(elf)]
    log_path.parent.mkdir(parents=True, exist_ok=True)

    proc = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    assert proc.stdout is not None

    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)

    deadline = time.monotonic() + args.timeout
    marker_status: str | None = None
    marker_line: str | None = None
    lines: list[str] = []

    try:
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                # Drain remaining buffered output before deciding.
                rest = proc.stdout.read()
                if rest:
                    for line in rest.splitlines(True):
                        lines.append(line)
                        sys.stdout.write(line)
                        match = MARKER.search(line)
                        if match and match.group("name") == args.test:
                            marker_status = match.group("status")
                            marker_line = match.group(0)
                break

            events = selector.select(timeout=0.25)
            for key, _ in events:
                line = key.fileobj.readline()
                if not line:
                    continue
                lines.append(line)
                sys.stdout.write(line)
                sys.stdout.flush()
                match = MARKER.search(line)
                if match and match.group("name") == args.test:
                    marker_status = match.group("status")
                    marker_line = match.group(0)
                    break

            if marker_status is not None:
                break
    finally:
        log_path.write_text("".join(lines), encoding="utf-8")

    if marker_status is not None:
        if proc.poll() is None:
            # The marker is the test oracle. Do not require the emulator UI/process
            # to choose to exit on its own.
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        print(f"HOST_RESULT {marker_line}")
        return 0 if marker_status == "PASS" else 1

    if proc.poll() is None:
        proc.kill()
        proc.wait()
        print(f"no SHADTEST marker before {args.timeout:.1f}s timeout", file=sys.stderr)
    else:
        print(f"shadPS4 exited with {proc.returncode} without a SHADTEST marker", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
