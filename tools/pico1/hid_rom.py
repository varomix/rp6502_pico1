#!/usr/bin/env python3
"""Pico 1 port: interactive keyboard / mouse / bell check on hardware.

  hid_rom.py <out_dir>

Echoes what is typed (the USB or BLE keyboard reaches the 6502 through the
console, RIA_RX), and once a frame prints the mouse's five XRAM bytes
(buttons, x, y, wheel, pan) when any changed. Ctrl+G echoes BEL, which
rings the RIA bell.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "tests" / "lib")]
from rp6502_asm import (RIA_READY, RIA_RX, RW0_ADDR, RW0_DATA, RW0_STEP,  # noqa: E402
                        Asm, putc, puthex, putnib)

VSYNC = 0xFFE3
MOUSE = 0xFF00
KEYBOARD = 0xFF10
PREV = 0x0210       # last mouse bytes shown
LASTVS = 0x0218
CHANGED = 0x0219


def hid():
    p = Asm()
    p.xreg(0, 0, 0, KEYBOARD)
    p.xreg(0, 0, 1, MOUSE)
    p.say("\r\nHID test: type, move and click the mouse, Ctrl+G rings the bell\r\n")

    p.symbol("loop")
    p.bit_abs(RIA_READY)            # V = a typed byte is waiting
    with p.branch("bvc"):
        p.lda_abs(RIA_RX)
        p.jsr_abs("putc")
        p.cmp_imm(0x0D)
        with p.branch("bne"):
            p.lda_imm(0x0A)
            p.jsr_abs("putc")
    p.lda_abs(VSYNC)
    p.cmp_abs(LASTVS)
    p.beq("loop")
    p.sta_abs(LASTVS)

    p.store(CHANGED, 0)
    p.store(RW0_STEP, 1)
    p.store(RW0_ADDR, MOUSE & 0xFF)
    p.store(RW0_ADDR + 1, MOUSE >> 8)
    p.ldx_imm(0)
    p.symbol("cmp")
    p.lda_abs(RW0_DATA)
    p.cmp_abx(PREV)
    with p.branch("beq"):
        p.sta_abx(PREV)
        p.inc_abs(CHANGED)
    p.inx()
    p.cpx_imm(5)
    p.bne("cmp")
    p.lda_abs(CHANGED)
    with p.branch("bne"):
        p.jmp_abs("loop")
    p.say("[M")
    p.ldx_imm(0)
    p.symbol("show")
    p.lda_imm(ord(" "))
    p.jsr_abs("putc")
    p.lda_abx(PREV)
    p.jsr_abs("puthex")
    p.inx()
    p.cpx_imm(5)
    p.bne("show")
    p.say("]\r\n")
    p.jmp_abs("loop")
    p.use(putc, puthex, putnib)
    return p


if __name__ == "__main__":
    out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    size = hid().write(out / "hid.rp6502")
    print(f"{out}/hid.rp6502  {size} bytes")
