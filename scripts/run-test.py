#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import sys
import time

ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*m")
CRITICAL = re.compile(r"<Critical>")
MARKER = re.compile(r"SHADTEST\s+name=(?P<name>\S+)\s+status=(?P<status>PASS|FAIL)\b(?P<detail>.*)")


def write_isolated_config(root: Path) -> Path:
    """Create the minimal host config required by the pixel-readback oracle."""
    user_dir = root / "shadPS4"
    user_dir.mkdir(parents=True, exist_ok=True)
    # On a fresh user dir shadPS4 creates home/1000 and unconditionally shows a
    # modal "Save Migration" SDL dialog (even with nothing to migrate), which
    # blocks unattended runs. Pre-creating the default user's home skips it.
    for sub in ("savedata", "trophy", "inputs"):
        (user_dir / "home" / "1000" / sub).mkdir(parents=True, exist_ok=True)
    # Current shadPS4 reads config.json. A lone legacy config.toml makes it pop a
    # modal "Config Migration" SDL dialog, which hangs an unattended run.
    config = user_dir / "config.json"
    config.write_text(
        json.dumps(
            {
                # Stage-0 oracle reads a GPU-written linear RT from guest CPU
                # memory, so make that readback requirement explicit.
                "GPU": {
                    "readbacks_mode": 2,
                    "readback_linear_images_enabled": True,
                },
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    return config


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("test", nargs="?", default="gpu_solid_rt")
    parser.add_argument("--shadps4", default=os.environ.get("SHADPS4"))
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument(
        "--use-host-config",
        action="store_true",
        help="do not isolate XDG_DATA_HOME or force the readback oracle settings",
    )
    parser.add_argument(
        "--allow-no-display",
        action="store_true",
        help="try anyway; current upstream shadPS4 normally needs SDL video",
    )
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    elf = root / "out" / args.test / f"{args.test}.elf"
    run_dir = root / "out" / args.test
    log_path = run_dir / "run.log"

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

    env = os.environ.copy()
    if not args.use_host_config:
        xdg_root = run_dir / "xdg-data"
        config = write_isolated_config(xdg_root)
        env["XDG_DATA_HOME"] = str(xdg_root)
        print(f"using isolated shadPS4 config: {config}")
        print("oracle settings: readbacksMode=Precise, readbackLinearImages=true")

    command = [args.shadps4, str(elf)]
    run_dir.mkdir(parents=True, exist_ok=True)

    # Unbuffered bytes: a buffered readline() can pull the marker into Python's
    # buffer behind an earlier line, and select() then never wakes up for it.
    proc = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
        env=env,
    )
    assert proc.stdout is not None
    stdout_fd = proc.stdout.fileno()

    selector = selectors.DefaultSelector()
    selector.register(stdout_fd, selectors.EVENT_READ)

    deadline = time.monotonic() + args.timeout
    marker_status: str | None = None
    marker_line: str | None = None
    critical_line: str | None = None
    lines: list[str] = []

    def consume(line: str) -> bool:
        """Record one output line; return True once the result marker is seen."""
        nonlocal marker_status, marker_line, critical_line
        lines.append(line)
        sys.stdout.write(line)
        sys.stdout.flush()
        plain = ANSI_ESCAPE.sub("", line)
        match = MARKER.search(plain)
        if match and match.group("name") == args.test:
            marker_status = match.group("status")
            marker_line = match.group(0)
            return True
        # A shadPS4 Critical (assert/unreachable) means the emulator is going
        # down; a guest marker printed after it can still read PASS.
        if critical_line is None and CRITICAL.search(plain):
            critical_line = plain.strip()
        return False

    pending = b""

    def consume_bytes(data: bytes, final: bool = False) -> bool:
        """Feed raw output; return True once the result marker is seen."""
        nonlocal pending
        pending += data
        *complete, pending = pending.split(b"\n")
        if final and pending:
            complete.append(pending)
            pending = b""
        for raw in complete:
            if consume(raw.decode("utf-8", "replace") + "\n"):
                return True
        return False

    try:
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                consume_bytes(proc.stdout.read() or b"", final=True)
                break

            if selector.select(timeout=0.25):
                chunk = os.read(stdout_fd, 65536)
                if chunk and consume_bytes(chunk):
                    break
    finally:
        log_path.write_text("".join(lines), encoding="utf-8")

    if marker_status is not None:
        if proc.poll() is None:
            # The marker is the test oracle. shadPS4's host-process exit status is
            # not the guest result contract.
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        if critical_line is not None:
            print(f"guest marker ignored: {marker_line}", file=sys.stderr)
            print(f"HOST_RESULT INFRA_FAIL shadPS4 critical before marker: {critical_line}")
            return 2
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
