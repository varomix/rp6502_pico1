#!/usr/bin/env python3
"""Pico 1 port: flash writes while a program runs, sound plays and video is on.

  cfgsave_rom.py <out_dir>

Starts the psg_rom.py eight-voice chord, opens AT0: and sends AT&W (save the
modem settings to littlefs) over and over, each once the modem has been
quiet for QUIET frames, printing each reply, any short write and the VSYNC
frame counter, so a stalled 6502, VSYNC or modem shows in the log. On the
RP2040 each flash program/erase runs with core 0's interrupts off.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "tests" / "lib"), str(ROOT / "tools" / "pico1")]
from rp6502_asm import (API_A, OP_OPEN, OP_READ_XSTACK, OP_WRITE_XSTACK,  # noqa: E402
                        O_RDWR, XSTACK, Asm, putc, puthex, putnib)
from psg_rom import chord, start  # noqa: E402

VSYNC = 0xFFE3
FD = 0x0200
WAITVS = 0x0201
SAVES = 0x0202
QUIET = 12          # frames of modem silence before the next command


def cfgsave():
    p = start()
    chord(p)
    p.push_str("AT0:")   # slot 0: AT&W saves only with a slot
    p.store(API_A, O_RDWR)
    p.call(OP_OPEN)
    p.sta_abs(FD)
    p.cmp_imm(0xFF)
    with p.branch("bne"):
        p.say("?AT: open failed\r\n")
        p.stp()
    p.store(SAVES, 0)

    p.symbol("again")
    # Wait until the modem has been quiet for QUIET frames, so a command is
    # never sent while the last one's reply is still coming.
    p.symbol("quiet")
    p.lda_abs(VSYNC)
    p.clc()
    p.adc_imm(QUIET)
    p.sta_abs(WAITVS)
    p.symbol("drain")
    p.push(0)
    p.push(1)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_READ_XSTACK)
    p.tax()
    with p.branch("beq"):
        p.lda_abs(XSTACK)
        p.jsr_abs("putc")
        p.jmp_abs("quiet")
    p.lda_abs(VSYNC)
    p.cmp_abs(WAITVS)
    p.bne("drain")

    for c in reversed(b"AT&W\r"):
        p.push(c)
    p.lda_abs(FD)
    p.sta_abs(API_A)
    p.call(OP_WRITE_XSTACK)
    p.cmp_imm(5)                    # bytes the modem took
    with p.branch("beq"):
        p.pha()
        p.say("[short ")
        p.pla()
        p.jsr_abs("puthex")
        p.say("]\r\n")
    p.inc_abs(SAVES)
    p.say("[save ")
    p.lda_abs(SAVES)
    p.jsr_abs("puthex")
    p.say(" vs ")
    p.lda_abs(VSYNC)
    p.jsr_abs("puthex")
    p.say("]\r\n")
    p.jmp_abs("again")
    p.use(putc, puthex, putnib)
    return p


if __name__ == "__main__":
    out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    size = cfgsave().write(out / "cfgsave.rp6502")
    print(f"{out}/cfgsave.rp6502  {size} bytes")
