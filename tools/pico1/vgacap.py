#!/usr/bin/env python3
"""Pico 1 port: grab what the VGA Pico actually renders, over its USB console.

Needs a VGA debug build (PICO1_DEBUG=1 tools/pico1/build.sh), which has
VGA_CAP (src/host/pico/vga/sys/cap.c).

  vgacap.py frame [--count N] [--out DIR]
      Sweep every canvas line and write N composited PNG frames. One image
      takes about a second, so its lines come from different video frames:
      a glitch that lasts one frame shows up as one wrong line.

  vgacap.py line Y [--frames N] [--out DIR]
      Capture canvas line Y over and over. Writes a strip PNG with one row per
      capture, top to bottom in time, plus a per-plane strip. Anything that
      flickers on that line shows up as rows that differ.

Writes PNGs with x2 scaling (--scale) and prints a summary of what differed.
Console text that arrives meanwhile is printed to stderr.
"""
import argparse, glob, os, select, struct, sys, termios, time, tty, zlib

MAGIC = b"VCAP"
HDR = 18


def png(path, w, h, rows, scale=1):
    """rows: list of h lists of w (r,g,b) tuples."""
    raw = bytearray()
    for row in rows:
        line = bytearray(b"\x00")
        for (r, g, b) in row:
            line += bytes((r, g, b)) * scale
        for _ in range(scale):
            raw += line
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 2, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


def rgb(p):
    r = p & 31
    g = (p >> 6) & 31
    b = (p >> 11) & 31
    return (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2)


def composite(planes, width):
    """planes: dict plane -> list of pixels. Plane 0 is the base; planes 1 and
    2 cover it where a pixel has the alpha bit (bit 5) set."""
    out = list(planes.get(0, [0] * width))
    for i in (1, 2):
        if i in planes:
            for x, p in enumerate(planes[i]):
                if p & 0x20:
                    out[x] = p
    return out


class Port:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        self.buf = bytearray()

    def send(self, cmd, arg=0):
        # Out of band: the command rides in the CDC line coding bit rate
        # (macOS IOSSIOSPEED sets any rate), then the rate goes back to a
        # plain one so the same command can be sent again.
        import fcntl
        IOSSIOSPEED = 0x80085402
        for rate in (0x5A000000 | cmd[0] << 16 | arg, 115200):
            fcntl.ioctl(self.fd, IOSSIOSPEED, struct.pack("L", rate))
            time.sleep(0.02)

    def fill(self, timeout):
        r, _, _ = select.select([self.fd], [], [], timeout)
        if r:
            self.buf += os.read(self.fd, 65536)
            return True
        return False

    def packets(self, timeout=3.0):
        """Yield (hdr dict, planes dict). Non-packet bytes go to stderr."""
        idle = time.time() + timeout
        while True:
            hits = [j for j in (self.buf.find(MAGIC), self.buf.find(b"VMEM")) if j >= 0]
            i = min(hits) if hits else -1
            if i >= 0 and self.buf[i:i + 4] == b"VMEM" and len(self.buf) >= i + HDR:
                if i:
                    sys.stderr.write(self.buf[:i].decode("latin1"))
                    del self.buf[:i]
                seq, block = struct.unpack_from("<2H", self.buf, 4)
                if len(self.buf) >= HDR + 1024:
                    data = bytes(self.buf[HDR:HDR + 1024])
                    del self.buf[:HDR + 1024]
                    idle = time.time() + timeout
                    yield dict(mem=block, seq=seq), data
                    continue
                if not self.fill(0.2) and time.time() > idle:
                    return
                continue
            if i < 0:
                keep = len(self.buf) - 3
                if keep > 0:
                    sys.stderr.write(self.buf[:keep].decode("latin1"))
                    del self.buf[:keep]
            else:
                if i:
                    sys.stderr.write(self.buf[:i].decode("latin1"))
                    del self.buf[:i]
                if len(self.buf) >= HDR:
                    seq, frame, line, width, height = struct.unpack_from("<5H", self.buf, 4)
                    mask, mode = self.buf[14], self.buf[15]
                    nplanes = bin(mask).count("1")
                    size = HDR + nplanes * width * 2
                    if width > 640 or height > 480 or line >= max(height, 1):
                        del self.buf[:4]  # not a real header
                        continue
                    if len(self.buf) >= size:
                        planes, off = {}, HDR
                        for p in range(3):
                            if mask & (1 << p):
                                planes[p] = list(struct.unpack_from("<%dH" % width, self.buf, off))
                                off += width * 2
                        del self.buf[:size]
                        idle = time.time() + timeout
                        yield dict(seq=seq, frame=frame, line=line, width=width,
                                   height=height, mask=mask), planes
                        continue
            if not self.fill(0.2) and time.time() > idle:
                return


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--out", default="vgacap")
    ap.add_argument("--scale", type=int, default=2)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("frame")
    f.add_argument("--count", type=int, default=1)
    f.add_argument("--stride", type=int, default=61)
    l = sub.add_parser("line")
    l.add_argument("y", type=int)
    l.add_argument("--frames", type=int, default=240)
    m = sub.add_parser("mem", help="read the VGA's XRAM, optionally diff with a .rp6502")
    m.add_argument("--rom")
    fp = sub.add_parser("flashpage", help="ask the VGA to stage XRAM[0..255] as flash page N")
    fp.add_argument("page", type=int)
    a = ap.parse_args()

    path = a.port or sorted(glob.glob("/dev/cu.usbmodem*"))[0]
    port = Port(path)
    os.makedirs(a.out, exist_ok=True)
    try:
        if a.cmd == "flashpage":
            port.send(b"F", a.page)
            end = time.time() + 3
            while time.time() < end:
                port.fill(0.2)
            sys.stdout.write(port.buf.decode("latin1"))
            return 0
        if a.cmd == "mem":
            vram = bytearray(65536)
            got = set()
            for first in range(0, 64, 16):
                port.send(b"M", first | (16 << 8))
                for hdr, data in port.packets():
                    if "mem" in hdr:
                        vram[hdr["mem"] * 1024:(hdr["mem"] + 1) * 1024] = data
                        got.add(hdr["mem"])
                        if hdr["mem"] == first + 15:
                            break
            out = os.path.join(a.out, "xram_vga.bin")
            open(out, "wb").write(vram)
            print("%s  %d/64 blocks" % (out, len(got)))
            if a.rom:
                d = open(a.rom, "rb").read()
                i = tot = 0
                bad = []
                while i < len(d):
                    j = d.index(b"\n", i)
                    line = d[i:j]
                    i = j + 1
                    if not line.startswith(b"$"):
                        continue
                    p = line.split()
                    addr, n = int(p[0][1:], 16), int(p[1][1:], 16)
                    chunk = d[i:i + n]
                    i += n
                    if addr < 0x10000:
                        continue
                    for k in range(n):
                        tot += 1
                        if vram[addr - 0x10000 + k] != chunk[k]:
                            bad.append((addr - 0x10000 + k, chunk[k], vram[addr - 0x10000 + k]))
                print("compared %d ROM bytes, %d differ" % (tot, len(bad)))
                for addr, want, have in bad[:40]:
                    print("  %04X rom %02X vga %02X  xor %02X" % (addr, want, have, want ^ have))
            return 0
        if a.cmd == "frame":
            port.send(b"S", a.stride)
            for n in range(a.count):
                lines, frames, t0 = {}, {}, time.time()
                width = height = None
                for hdr, planes in port.packets():
                    width, height = hdr["width"], hdr["height"]
                    lines[hdr["line"]] = composite(planes, width)
                    frames[hdr["line"]] = (hdr["frame"], hdr["mask"])
                    if len(lines) >= height:
                        break
                if not lines:
                    print("no capture data (is this a VGA_CAP build?)")
                    return 1
                rows = [[rgb(p) for p in lines.get(y, [0] * width)] for y in range(height)]
                out = os.path.join(a.out, "frame%03d.png" % n)
                png(out, width, height, rows, a.scale)
                masks = sorted(set(m for _, m in frames.values()))
                print("%s  %dx%d  %d lines in %.1fs  plane masks %s" % (
                    out, width, height, len(lines), time.time() - t0, masks))
        else:
            port.send(b"L", a.y)
            caps = []
            for hdr, planes in port.packets():
                caps.append((hdr, planes))
                if len(caps) >= a.frames:
                    break
            if not caps:
                print("no capture data (is this a VGA_CAP build?)")
                return 1
            width = caps[0][0]["width"]
            rows = [[rgb(p) for p in composite(pl, width)] for _, pl in caps]
            out = os.path.join(a.out, "line%03d.png" % a.y)
            png(out, width, len(rows), rows, a.scale)
            # per plane: plane 0 | plane 1 | plane 2 side by side, black if unfilled
            prow = []
            for _, pl in caps:
                r = []
                for p in range(3):
                    r += [rgb(x) for x in pl.get(p, [0] * width)] + [(255, 0, 255)] * 2
                prow.append(r)
            outp = os.path.join(a.out, "line%03d_planes.png" % a.y)
            png(outp, len(prow[0]), len(prow), prow, 1)
            # how many distinct versions of the line, and where they differ
            comps = [tuple(composite(pl, width)) for _, pl in caps]
            distinct = {}
            for i, c in enumerate(comps):
                distinct.setdefault(c, []).append(i)
            fr = [h["frame"] for h, _ in caps]
            gaps = sum(1 for i in range(1, len(fr)) if (fr[i] - fr[i - 1]) & 0xFFFF != 1)
            print("%s  %d captures, frames %d..%d (%d gaps), %d distinct versions" % (
                out, len(caps), fr[0], fr[-1], gaps, len(distinct)))
            base = max(distinct, key=lambda k: len(distinct[k]))
            for c, idx in sorted(distinct.items(), key=lambda kv: -len(kv[1]))[:8]:
                diff = [x for x in range(width) if c[x] != base[x]]
                span = "x %d..%d (%d px)" % (diff[0], diff[-1], len(diff)) if diff else "baseline"
                print("  %4d captures  %s  masks %s" % (
                    len(idx), span, sorted(set(caps[i][0]["mask"] for i in idx))))
            print(outp)
    finally:
        port.send(b"0")
        time.sleep(0.2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
