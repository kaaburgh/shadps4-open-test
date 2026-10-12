#!/usr/bin/env python3
"""Presses one key in the X display named by $DISPLAY every few seconds.

hle_ime closes each IME panel with the PS4 Circle button, which shadPS4's
default keyboard mapping puts on "b". The pointer is moved to the screen
centre first: without a window manager, X sends key events to the window
under the pointer.

usage: press_keys.py [key] [interval_s] [duration_s]
"""
import ctypes
import sys
import time

key = (sys.argv[1] if len(sys.argv) > 1 else "b").encode()
interval = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
duration = float(sys.argv[3]) if len(sys.argv) > 3 else 120.0

x11 = ctypes.CDLL("libX11.so.6")
xtst = ctypes.CDLL("libXtst.so.6")
x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
x11.XStringToKeysym.restype = ctypes.c_ulong
x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
x11.XKeysymToKeycode.restype = ctypes.c_ubyte
x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x11.XFlush.argtypes = [ctypes.c_void_p]
x11.XDefaultScreen.argtypes = [ctypes.c_void_p]
x11.XDisplayWidth.argtypes = [ctypes.c_void_p, ctypes.c_int]
x11.XDisplayHeight.argtypes = [ctypes.c_void_p, ctypes.c_int]
xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
xtst.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                      ctypes.c_ulong]

dpy = None
for _ in range(100):
    dpy = x11.XOpenDisplay(None)
    if dpy:
        break
    time.sleep(0.1)
if not dpy:
    sys.exit("cannot open display")
keycode = x11.XKeysymToKeycode(dpy, x11.XStringToKeysym(key))
screen = x11.XDefaultScreen(dpy)
end = time.monotonic() + duration
while time.monotonic() < end:
    time.sleep(interval)
    xtst.XTestFakeMotionEvent(dpy, -1, x11.XDisplayWidth(dpy, screen) // 2,
                              x11.XDisplayHeight(dpy, screen) // 2, 0)
    xtst.XTestFakeKeyEvent(dpy, keycode, 1, 0)
    x11.XFlush(dpy)
    time.sleep(0.15)
    xtst.XTestFakeKeyEvent(dpy, keycode, 0, 0)
    x11.XFlush(dpy)
