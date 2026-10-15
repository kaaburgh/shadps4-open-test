#!/usr/bin/env python3
"""Captures present_transfer's frame from the X display and checks its pixels.

capture.py capture OUT.json [timeout_s]
    Run on the test's display (scripts/run-test-lavapipe.sh XVFB_HOOK). Polls
    the root window with XGetImage until the 32 patch centres are not all
    black and two captures a second apart agree, then writes the centre
    pixels to OUT.json. shadPS4 opens its window at the top left of the
    1280x720 screen, at the frame's size, so frame and screen pixels match.

capture.py check OUT.json default|rec709
    Compares the captured centres with tools/patches.py's reference for that
    output mode, within 1 LSB. Prints PRESENT_TRANSFER mode=... PASS|FAIL and
    exits 0 or 1.
"""
from __future__ import annotations

import ctypes
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import patches  # noqa: E402

TOLERANCE = 1
SAMPLE = 8  # average an 8x8 block around each patch centre


def grab(dpy, x11, root, width, height):
    image = x11.XGetImage(dpy, root, 0, 0, width, height, ctypes.c_ulong(~0), 2)  # ZPixmap
    if not image:
        return None
    base = ctypes.cast(image, ctypes.POINTER(ctypes.c_uint8))
    data_ptr = ctypes.cast(ctypes.c_void_p.from_address(image + 16).value,
                           ctypes.POINTER(ctypes.c_uint8))
    bytes_per_line = ctypes.c_int.from_address(image + 44).value
    bits_per_pixel = ctypes.c_int.from_address(image + 48).value
    del base
    if bits_per_pixel != 32:
        sys.exit(f"unexpected {bits_per_pixel} bits per pixel")
    centres = []
    for index in range(len(patches.PATCHES)):
        cx, cy = patches.patch_centre(index)
        acc = [0, 0, 0]
        for y in range(cy - SAMPLE // 2, cy + SAMPLE // 2):
            row = y * bytes_per_line
            for x in range(cx - SAMPLE // 2, cx + SAMPLE // 2):
                o = row + x * 4  # B, G, R, X
                acc[0] += data_ptr[o + 2]
                acc[1] += data_ptr[o + 1]
                acc[2] += data_ptr[o]
        n = SAMPLE * SAMPLE
        centres.append([round(c / n, 2) for c in acc])
    return centres


def capture(out: Path, timeout: float) -> int:
    x11 = ctypes.CDLL("libX11.so.6")
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XDefaultRootWindow.restype = ctypes.c_ulong
    x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x11.XGetImage.restype = ctypes.c_void_p
    x11.XGetImage.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                              ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_int]
    deadline = time.monotonic() + timeout
    dpy = None
    while not dpy and time.monotonic() < deadline:
        dpy = x11.XOpenDisplay(None)
        time.sleep(0.2)
    if not dpy:
        return 2
    root = x11.XDefaultRootWindow(dpy)
    previous = None
    while time.monotonic() < deadline:
        time.sleep(1.0)
        centres = grab(dpy, x11, root, patches.WIDTH, patches.HEIGHT)
        if centres is None or max(max(c) for c in centres) == 0:
            continue
        if centres == previous:
            out.write_text(json.dumps({"centres": centres}) + "\n", encoding="utf-8")
            return 0
        previous = centres
    return 1


def check(out: Path, mode: str) -> int:
    if not out.is_file():
        print(f"PRESENT_TRANSFER mode={mode} FAIL reason=no_capture")
        return 1
    centres = json.loads(out.read_text(encoding="utf-8"))["centres"]
    worst = (0.0, -1, "")
    for index, (rgb, got) in enumerate(zip(patches.PATCHES, centres)):
        want = [patches.reference(c, mode == "rec709") for c in rgb]
        for channel, (w, g) in enumerate(zip(want, got)):
            err = abs(g - w)
            if err > worst[0]:
                worst = (err, index, f"patch={index} rgb={rgb} channel={'RGB'[channel]} "
                                     f"expected={w} got={g}")
    status = "PASS" if worst[0] <= TOLERANCE else "FAIL"
    detail = f" max_error={worst[0]:g}" + (f" {worst[2]}" if worst[1] >= 0 else "")
    print(f"PRESENT_TRANSFER mode={mode} {status}{detail}")
    return 0 if status == "PASS" else 1


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "capture":
        sys.exit(capture(Path(sys.argv[2]), float(sys.argv[3]) if len(sys.argv) > 3 else 60.0))
    if len(sys.argv) == 4 and sys.argv[1] == "check":
        sys.exit(check(Path(sys.argv[2]), sys.argv[3]))
    sys.exit(__doc__)
