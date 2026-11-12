#!/usr/bin/env python3
"""Pico 1 port: a modem terminal ROM for testing the RIA-W's AT: device.

  modem_rom.py <out_dir>

The 6502 opens AT: and copies bytes both ways, one at a time: console in
(RIA_RX) to the modem (retrying a write that takes 0 bytes), modem out to the console (RIA_TX). Type AT commands
on the console, e.g. ATDT10.0.0.5:2323 to dial a TCP listener, +++ then
ATH to hang up. Reads of AT: never block, so neither side starves.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "tests" / "lib")]
from rp6502_asm import (API_A, OP_OPEN, OP_READ_XSTACK, OP_WRITE_XSTACK,  # noqa: E402
                        O_RDWR, RIA_READY, RIA_RX, XSTACK, Asm, putc)

FD = 0x0200
BYTE = 0x0201


def modem():
    p = Asm()
    p.push_str("AT:")
    p.store(API_A, O_RDWR)
    p.call(OP_OPEN)
    p.sta_abs(FD)
    p.cmp_imm(0xFF)
    with p.branch("bne"):
        p.say("?AT: open failed\r\n")
        p.stp()
    p.say("AT: open\r\n")

    p.symbol("loop")
    p.bit_abs(RIA_READY)            # V = a console byte is waiting
    with p.branch("bvc"):
        p.lda_abs(RIA_RX)
        p.sta_abs(BYTE)
        p.symbol("send")            # a full TCP send queue writes 0: retry
        p.lda_abs(BYTE)
        p.sta_abs(XSTACK)
        p.lda_abs(FD)
        p.sta_abs(API_A)
        p.call(OP_WRITE_XSTACK)
        p.tax()
        p.beq("send")
    p.push(0)
    p.push(1)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)
    p.tax()
    with p.branch("beq"):
        p.lda_abs(XSTACK)
        p.jsr_abs("putc")
    p.jmp_abs("loop")
    p.use(putc)
    return p


if __name__ == "__main__":
    out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    size = modem().write(out / "modem.rp6502")
    print(f"{out}/modem.rp6502  {size} bytes")
