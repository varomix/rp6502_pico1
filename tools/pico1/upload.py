#!/usr/bin/env python3
"""Pico 1 port: send a file to the Picocomputer's drive with the monitor's
UPLOAD command over the VGA USB console, optionally then run monitor commands
(e.g. FLASH). Stdlib only.

  upload.py local_file remote_path [--then "FLASH remote_path"]
"""
import argparse, glob, os, select, sys, termios, time, tty, zlib

ap = argparse.ArgumentParser()
ap.add_argument("local"); ap.add_argument("remote")
ap.add_argument("--port"); ap.add_argument("--then", action="append", default=[])
a = ap.parse_args()
port = a.port or sorted(glob.glob("/dev/cu.usbmodem*"))[0]
fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)
t = termios.tcgetattr(fd); t[4] = t[5] = termios.B115200; termios.tcsetattr(fd, termios.TCSANOW, t)

def read_until(marks, timeout):
    buf, end = b"", time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try: buf += os.read(fd, 4096)
            except BlockingIOError: pass
            for m in marks:
                if m in buf: return buf, m
    return buf, None

def write_all(data):
    view = memoryview(data)
    while view:
        try: n = os.write(fd, view); view = view[n:]
        except BlockingIOError: select.select([], [fd], [], 0.5)

read_until([b"\xff"], 0.3)  # drain
write_all(b"\r"); read_until([b"]"], 2)
# After the prompt the monitor probes the terminal (ESC[6n, ESC[>c) and reads
# the reply for a moment; keys typed then are eaten as the reply. Let it settle.
read_until([b"\xff"], 0.6)
write_all(f"UPLOAD {a.remote}\r".encode())
out, m = read_until([b"}", b"\n?"], 5)
if m != b"}": sys.exit(f"UPLOAD refused: {out!r}")
data = open(a.local, "rb").read()
t0 = time.time()
for off in range(0, len(data), 1024):
    chunk = data[off:off + 1024]
    write_all(f"${len(chunk):X} ${zlib.crc32(chunk):08X}\r".encode())
    time.sleep(0.01)
    write_all(chunk)
    out, m = read_until([b"}", b"\n?"], 10)
    if m != b"}":
        sys.exit(f"\nchunk at {off} failed: {out[-1200:]!r}")
    done = off + len(chunk)
    sys.stdout.write(f"\r{done}/{len(data)} bytes  {done/(time.time()-t0)/1024:.1f} KB/s"); sys.stdout.flush()
write_all(b"END\r"); read_until([b"]"], 5)
print("\nupload complete")
for c in a.then:
    write_all(c.encode() + b"\r")
    out, _ = read_until([b"\xff"], 30)
    print(out.decode("utf-8", "replace"))
os.close(fd)
