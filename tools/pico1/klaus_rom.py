#!/usr/bin/env python3
"""Pico 1 port: wrap Klaus Dormann's 6502 / 65C02 functional tests as .rp6502
ROMs for a soak on real hardware.

  klaus_rom.py <out_dir>

Each test's success trap (`jmp *`) is patched to jump to a reporter that
prints "<name> PASS <count>" on the console and restarts the test, so a
healthy machine prints a new line every pass, forever. A failure leaves the
6502 spinning in one of the test's error traps: the lines stop, and the
current test number is in $0200 (test_case).
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "tests" / "lib")]
import rp6502  # noqa: E402
from rp6502_asm import Asm, putc, putnib, puthex  # noqa: E402

BINS = ROOT / "vendor" / "6502_65C02_functional_tests" / "bin_files"
REPORT = 0x3E00   # above both images (they end below $3900)
COUNT = 0x3F00    # 16-bit pass counter
START = 0x0400

TESTS = {
    # name: (image, success trap address from the .lst)
    "klaus6502": ("6502_functional_test.bin", 0x3469),
    "klaus65c02": ("65C02_extended_opcodes_test.bin", 0x24F1),
}


def reporter(name):
    p = Asm(REPORT)
    p.jmp_abs("main")
    p.use(putc, putnib, puthex)
    p.symbol("main")
    p.cld()
    p.inc_abs(COUNT)
    with p.branch("bne"):
        p.inc_abs(COUNT + 1)
    p.say(f"{name} PASS ")
    p.lda_abs(COUNT + 1)
    p.jsr_abs("puthex")
    p.lda_abs(COUNT)
    p.jsr_abs("puthex")
    p.say("\r\n")
    p.jmp_abs(START)
    return p.code()


def build(name, image, trap, out):
    mem = bytearray((BINS / image).read_bytes())
    assert mem[trap:trap + 3] == bytes([0x4C, trap & 0xFF, trap >> 8]), \
        f"{image}: no `jmp *` at ${trap:04X}"
    mem[trap + 1:trap + 3] = bytes([REPORT & 0xFF, REPORT >> 8])
    rom = rp6502.ROM()
    # Zero page through the end of the image ($FF fill above it is unused).
    end = max(i for i, b in enumerate(mem[:0xFF00]) if b not in (0x00, 0xFF)) + 1
    rom.add_binary_data(bytes(mem[0:end]), 0)
    rom.add_binary_data(reporter(name), REPORT)
    rom.add_binary_data(bytes(2), COUNT)
    # The image's own NMI/IRQ vectors point at its traps; reset starts the test.
    rom.add_nmi_vector(mem[0xFFFA] | mem[0xFFFB] << 8)
    rom.add_reset_vector(START)
    rom.add_irq_vector(mem[0xFFFE] | mem[0xFFFF] << 8)
    path = Path(out) / f"{name}.rp6502"
    rom.write(path)
    print(f"{path}  image $0000-${end - 1:04X}, trap ${trap:04X} -> ${REPORT:04X}")


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    for name, (image, trap) in TESTS.items():
        build(name, image, trap, out)
