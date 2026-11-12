#!/usr/bin/env python3
"""Pico 1 port: run VGA test ROMs on the board and report the VGA's missed
scanlines and render times for each, per display type.

  vgarun.py [--types 0,1,2] [--secs 5] [--dir MSC0:/pico1test/vm] rom ...

Each rom is a name on the drive (without .rp6502). For every display type
(SET VGA n) and rom: break the 6502, LOAD the rom, and read the VGA's debug
lines ("[vga miss=... max0=..us max1=..us ...]", printed every 2 s on its
USB console) for --secs seconds. The display type is put back to 0 at the
end. Prints one line per run; a run with misses is marked "MISS".
The VGA must run a debug build (PICO1_DEBUG=1 tools/pico1/build.sh).
"""
import argparse, glob, os, re, select, termios, time, tty

ap = argparse.ArgumentParser()
ap.add_argument("--types", default="0,1,2")
ap.add_argument("--secs", type=float, default=5.0)
ap.add_argument("--dir", default="MSC0:/pico1test/vm")
ap.add_argument("--port")
ap.add_argument("roms", nargs="+")
a = ap.parse_args()

port = a.port or sorted(glob.glob("/dev/cu.usbmodem*"))[0]
fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)
t = termios.tcgetattr(fd); t[4] = t[5] = termios.B115200
termios.tcsetattr(fd, termios.TCSANOW, t)


def read_for(s):
    out, end = b"", time.time() + s
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                out += os.read(fd, 65536)
            except BlockingIOError:
                pass
    return out.decode("utf-8", "replace")


def monitor(cmd, wait=1.5):
    termios.tcsendbreak(fd, 0)
    read_for(1.2)
    os.write(fd, b"\r")
    read_for(0.8)  # the prompt's terminal probe eats keys typed meanwhile
    os.write(fd, cmd.encode() + b"\r")
    return read_for(wait)


VGA = re.compile(r"\[vga miss=(\d+) \+(\d+) max0=(\d+)us max1=(\d+)us")
worst = {}
try:
    for vt in [int(x) for x in a.types.split(",")]:
        monitor(f"SET VGA {vt}", 3.0)
        for rom in a.roms:
            out = monitor(f"LOAD {a.dir}/{rom}.rp6502", a.secs)
            lines = VGA.findall(out)[1:]  # the first window spans the load
            if not lines:
                print(f"vga={vt} {rom:28s} no VGA lines", flush=True)
                continue
            miss = sum(int(m[1]) for m in lines)
            m0 = max(int(m[2]) for m in lines)
            m1 = max(int(m[3]) for m in lines)
            err = "" if "?" not in out.split("LOAD", 1)[-1][:200] else \
                "  load error? " + out.split("LOAD", 1)[-1][:80].replace("\r\n", " ")
            flag = "MISS" if miss else "ok"
            print(f"vga={vt} {rom:28s} {flag:4s} +miss={miss:<6d} max0={m0:3d}us max1={m1:3d}us{err}",
                  flush=True)
            w = worst.setdefault(vt, [0, 0, 0])
            w[0] += miss; w[1] = max(w[1], m0); w[2] = max(w[2], m1)
finally:
    monitor("SET VGA 0", 3.0)
    os.close(fd)
for vt, (miss, m0, m1) in sorted(worst.items()):
    print(f"== vga={vt}: total new misses {miss}, worst render max0={m0}us max1={m1}us")
