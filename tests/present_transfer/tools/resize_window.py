#!/usr/bin/env python3
"""Resizes shadPS4's window through a list of sizes, as a user dragging it would.

resize_window.py [--hold SECONDS] WxH [WxH ...]
    Run on the test's display (scripts/run-test-lavapipe.sh XVFB_HOOK). Waits
    for a top-level window whose name contains "shadPS4", then resizes it to
    each size in turn and holds it there for --hold seconds (default 1.5).
    Prints RESIZE lines and exits 0, or 1 if the window never appeared.
"""
from __future__ import annotations

import argparse
import ctypes
import time


def find_window(dpy, x11, root):
    root_ret, parent_ret = ctypes.c_ulong(), ctypes.c_ulong()
    children, count = ctypes.POINTER(ctypes.c_ulong)(), ctypes.c_uint()
    if not x11.XQueryTree(dpy, root, ctypes.byref(root_ret), ctypes.byref(parent_ret),
                          ctypes.byref(children), ctypes.byref(count)):
        return None
    found = None
    for i in range(count.value):
        name = ctypes.c_char_p()
        if x11.XFetchName(dpy, children[i], ctypes.byref(name)) and name.value:
            if b"shadPS4" in name.value:
                found = children[i]
            x11.XFree(name)
    if children:
        x11.XFree(children)
    return found


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--hold", type=float, default=1.5)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("sizes", nargs="+")
    args = parser.parse_args()

    x11 = ctypes.CDLL("libX11.so.6")
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XDefaultRootWindow.restype = ctypes.c_ulong
    x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x11.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p,
                               ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    x11.XFetchName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p]
    x11.XFree.argtypes = [ctypes.c_void_p]
    x11.XResizeWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_uint, ctypes.c_uint]
    x11.XFlush.argtypes = [ctypes.c_void_p]

    deadline = time.monotonic() + args.timeout
    dpy = None
    while not dpy and time.monotonic() < deadline:
        dpy = x11.XOpenDisplay(None)
        time.sleep(0.2)
    if not dpy:
        return 1
    root = x11.XDefaultRootWindow(dpy)
    window = None
    while window is None and time.monotonic() < deadline:
        window = find_window(dpy, x11, root)
        time.sleep(0.5)
    if window is None:
        print("RESIZE no shadPS4 window", flush=True)
        return 1
    # Let the first frames reach the screen before the first resize.
    time.sleep(args.hold)
    for size in args.sizes:
        width, height = (int(v) for v in size.split("x"))
        x11.XResizeWindow(dpy, window, width, height)
        x11.XFlush(dpy)
        print(f"RESIZE {width}x{height}", flush=True)
        time.sleep(args.hold)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
