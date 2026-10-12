#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import subprocess
import sys
import time

ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*m")
CRITICAL = re.compile(r"<Critical>")
MARKER = re.compile(r"SHADTEST\s+name=(?P<name>\S+)\s+status=(?P<status>PASS|FAIL)\b(?P<detail>.*)")


READBACKS_MODES = {"disabled": 0, "relaxed": 1, "precise": 2}


def merge_settings(config: dict, overlay: dict) -> None:
    """Merge {"Section": {"key": value}} settings into config, section by section."""
    for section, values in overlay.items():
        config.setdefault(section, {}).update(values)


def parse_set(text: str) -> tuple[str, str, object]:
    """Parse --set Section.key=value; value is JSON, or a plain string."""
    name, sep, raw = text.partition("=")
    section, dot, key = name.partition(".")
    if not sep or not dot or not section or not key:
        raise argparse.ArgumentTypeError(f"expected Section.key=value, got {text!r}")
    try:
        value = json.loads(raw)
    except json.JSONDecodeError:
        value = raw
    return section, key, value


def write_isolated_config(root: Path, readbacks: str, overlay: dict | None = None) -> Path:
    """Create the minimal host config required by the pixel-readback oracle.

    overlay adds test-specific settings, from out/<test>/config.json and --set."""
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
    settings = {
        # Stage-0 oracle reads a GPU-written linear RT from guest CPU
        # memory, so make that readback requirement explicit. Other
        # readbacks modes are for tests that compare configurations.
        "GPU": {
            "readbacks_mode": READBACKS_MODES[readbacks],
            "readback_linear_images_enabled": True,
        },
    }
    merge_settings(settings, overlay or {})
    config.write_text(json.dumps(settings, indent=2) + "\n", encoding="utf-8")
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
        "--readbacks",
        choices=sorted(READBACKS_MODES),
        default="precise",
        help="readbacks mode written to the isolated config (default: precise)",
    )
    parser.add_argument(
        "--shared-backing",
        action="store_true",
        help="set SHADPS4_UMA_SHARED_BACKING=1 (UMA E3 research builds of shadPS4)",
    )
    parser.add_argument(
        "--set",
        action="append",
        default=[],
        type=parse_set,
        metavar="SECTION.KEY=VALUE",
        help="extra shadPS4 setting for the isolated config, e.g. Audio.audio_backend=1",
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

    if args.use_host_config and (args.readbacks != "precise" or args.set):
        print("--readbacks and --set need the isolated config; drop --use-host-config",
              file=sys.stderr)
        return 2

    # Optional per-test inputs, written by the test's Makefile:
    #   out/<test>/config.json    settings merged into the isolated config;
    #   out/<test>/user/          files copied into the isolated shadPS4 user
    #                             directory, e.g. trophy data;
    #   out/<test>/forbid-log.txt one regular expression per line; a matching
    #                             shadPS4 log line fails the run.
    overlay: dict = {}
    overlay_file = run_dir / "config.json"
    if overlay_file.is_file():
        merge_settings(overlay, json.loads(overlay_file.read_text(encoding="utf-8")))
    for section, key, value in args.set:
        merge_settings(overlay, {section: {key: value}})
    forbidden: list[re.Pattern[str]] = []
    forbid_file = run_dir / "forbid-log.txt"
    if forbid_file.is_file():
        for pattern in forbid_file.read_text(encoding="utf-8").splitlines():
            if pattern.strip() and not pattern.startswith("#"):
                forbidden.append(re.compile(pattern))

    env = os.environ.copy()
    # The flag is the only switch: an exported variable would otherwise turn
    # every "mirror" run of run-matrix.sh into a shared one.
    if args.shared_backing:
        env["SHADPS4_UMA_SHARED_BACKING"] = "1"
    else:
        env.pop("SHADPS4_UMA_SHARED_BACKING", None)
    if not args.use_host_config:
        xdg_root = run_dir / "xdg-data"
        # Start from scratch: shadPS4 and the test's user files write here.
        shutil.rmtree(xdg_root, ignore_errors=True)
        config = write_isolated_config(xdg_root, args.readbacks, overlay)
        user_files = run_dir / "user"
        if user_files.is_dir():
            shutil.copytree(user_files, xdg_root / "shadPS4", dirs_exist_ok=True)
        env["XDG_DATA_HOME"] = str(xdg_root)
        print(f"using isolated shadPS4 config: {config}")
        print(
            f"oracle settings: readbacksMode={args.readbacks.capitalize()}, "
            "readbackLinearImages=true"
        )
    print(f"shared backing: {'on' if env.get('SHADPS4_UMA_SHARED_BACKING') == '1' else 'off'}")

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
    forbidden_line: str | None = None
    lines: list[str] = []

    def consume(line: str) -> bool:
        """Record one output line; return True once the result marker is seen."""
        nonlocal marker_status, marker_line, critical_line, forbidden_line
        lines.append(line)
        sys.stdout.write(line)
        sys.stdout.flush()
        plain = ANSI_ESCAPE.sub("", line)
        if forbidden_line is None and any(p.search(plain) for p in forbidden):
            forbidden_line = plain.strip()
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
        if forbidden_line is not None:
            print(f"guest marker ignored: {marker_line}", file=sys.stderr)
            print(f"HOST_RESULT FAIL forbidden shadPS4 log line: {forbidden_line}")
            return 1
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
