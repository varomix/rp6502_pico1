#!/usr/bin/env python3
"""Pico 1 port: PSG load ROMs for measuring the audio IRQ on the RIA.

  psg_rom.py <out_dir>

Each ROM takes the PSG (xreg 0,1,0 -> XRAM $8000) and spins or loops, so
the RP2040 debug line [AUD ...] shows the cost per 49.7 kHz sample:

  psg0      engine on, no notes
  psg8      a C major chord on all eight channels: 2 sine, 2 triangle,
            2 square, 2 sawtooth
  psgsweep  psg8, then the 6502 rewrites every channel's frequency in a
            loop, so the engine re-divides its phase increments constantly
  psgleft   one 440 Hz sine panned hard left (speaker check)
  psgright  the same panned hard right
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "tests" / "lib")]
from rp6502_asm import RW0_ADDR, RW0_DATA, Asm  # noqa: E402

PAGE = 0x8000
# Hertz; the freq register holds hertz times three.
NOTES = (131, 165, 196, 262, 330, 392, 523, 659)
# Waveform nibble: 0 sine, 1 square, 2 sawtooth, 3 triangle.
WAVES = (0, 0, 3, 3, 1, 1, 2, 2)


def chord(p):
    for ch, (hz, wave) in enumerate(zip(NOTES, WAVES)):
        base = PAGE + ch * 8
        f = hz * 3
        duty = 0x80 if wave == 1 else 0xFF
        for off, val in (
            (0, f & 0xFF), (1, f >> 8),
            (2, duty),
            (3, 0x50),          # volume 5 (of 0 loudest .. 15 silent), fast attack
            (4, 0x50),          # sustain at volume 5, fast decay
            (5, wave << 4 | 4), # waveform, medium release
            (6, 0x01),          # centred, gate on
        ):
            p.poke(base + off, val)


def start():
    p = Asm()
    p.xreg(0, 1, 0, PAGE)
    # psg.sv releases its voices up to a sample after the pointer is set;
    # wait a little so the gates below start their notes.
    loop = p.local("settle")
    p.ldx_imm(0)
    p.symbol(loop)
    p.dex()
    p.bne(loop)
    return p


def spin(p):
    p.jmp_abs(p.here())
    return p


def psg0():
    return spin(start())


def psg8():
    p = start()
    chord(p)
    return spin(p)


def psgsweep():
    p = start()
    chord(p)
    # Bump each channel's freq low byte in turn, forever: a warble whose
    # every write changes a frequency the engine must re-divide.
    loop = p.local("sweep")
    p.symbol(loop)
    for ch in range(8):
        p.store(RW0_ADDR, (PAGE + ch * 8) & 0xFF)
        p.store(RW0_ADDR + 1, PAGE >> 8)
        p.inc_abs(0x0300 + ch)
        p.lda_abs(0x0300 + ch)
        p.sta_abs(RW0_DATA)
    p.jmp_abs(loop)
    return p


def tone(pan_gate):
    """One 440 Hz sine on channel 0. pan_gate is the pan (signed, -63 left
    to 63 right) shifted left one, with the gate in bit 0."""
    p = start()
    f = 440 * 3
    for off, val in ((0, f & 0xFF), (1, f >> 8), (2, 0xFF), (3, 0x30), (4, 0x30),
                     (5, 0x04), (6, pan_gate)):
        p.poke(PAGE + off, val)
    return spin(p)


def psgleft():
    return tone(0x83)  # pan -62: left 125/128, right 1/128


def psgright():
    return tone(0x7F)  # pan 63: right only


ROMS = {f.__name__: f for f in (psg0, psg8, psgsweep, psgleft, psgright)}

if __name__ == "__main__":
    out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    for name, make in ROMS.items():
        size = make().write(out / f"{name}.rp6502")
        print(f"{out / name}.rp6502  {size} bytes")
