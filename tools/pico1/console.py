#!/usr/bin/env python3
"""Pico 1 port: talk to the Picocomputer monitor over the VGA's USB CDC port.

  console.py [--port /dev/cu.usbmodemXXXX] [--wait SECONDS] "command" ...

Each command is sent followed by CR; output is printed until the line goes
quiet for --wait seconds. With no commands, just prints what arrives.
Stdlib only (termios), so no pyserial needed.
"""
import argparse, glob, os, select, sys, termios, time, tty

ap = argparse.ArgumentParser()
ap.add_argument("--port")
ap.add_argument("--wait", type=float, default=0.8,
                help="seconds of quiet that end a command; under 1 s, as the VGA prints a debug line every second")
ap.add_argument("--max", type=float, default=30, help="max seconds per command")
ap.add_argument("cmds", nargs="*")
a = ap.parse_args()
port = a.port or (sorted(glob.glob("/dev/cu.usbmodem*")) or [None])[0]
if not port:
    sys.exit("no /dev/cu.usbmodem* found")
fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)
attrs = termios.tcgetattr(fd)
attrs[4] = attrs[5] = termios.B115200
termios.tcsetattr(fd, termios.TCSANOW, attrs)

def drain(quiet, limit):
    out, last, start = b"", time.time(), time.time()
    while time.time() - last < quiet and time.time() - start < limit:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                continue
            if chunk:
                out += chunk
                last = time.time()
    return out.decode("utf-8", "replace")

sys.stdout.write(drain(0.3, 2))
for c in a.cmds:
    os.write(fd, c.encode() + b"\r")
    sys.stdout.write(drain(a.wait, a.max))
    sys.stdout.flush()
os.close(fd)
