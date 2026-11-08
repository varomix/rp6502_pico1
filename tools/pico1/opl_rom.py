#!/usr/bin/env python3
"""Pico 1 port: OPL load ROMs for measuring the audio IRQ on the RIA.

  opl_rom.py <out_dir>

Each ROM takes the OPL (xreg 0,1,1 -> page $F000), sets up voices, and
spins, so the RP2040 debug line [AUD ...] on the console shows the cost
of that load per 49.7 kHz sample:

  opl0     engine on, no notes
  opl1     one held note
  opl9     all nine melodic channels held
  oplrhy   rhythm mode: six melodic channels + the five drums held on
  oplspam  opl9, then the 6502 rewrites one register as fast as it can
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "tests" / "lib")]
from rp6502_asm import RW0_ADDR, RW0_DATA, RW0_STEP, Asm  # noqa: E402

PAGE = 0xF000
# Modulator operator offset of each channel; the carrier is +3.
MOD = (0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12)
# F-numbers (block 4) for a spread of notes so the channels differ.
FNUM = (0x158, 0x183, 0x1B2, 0x1CA, 0x202, 0x241, 0x287, 0x2AE, 0x2D6)


def voice(p, ch, key=True):
    m = MOD[ch]
    for reg, val in (
        (0x20 + m, 0x21), (0x23 + m, 0x21),  # sustain EG, multiple 1
        (0x40 + m, 0x10), (0x43 + m, 0x04),  # levels
        (0x60 + m, 0xF2), (0x63 + m, 0xF2),  # fast attack, slow decay
        (0x80 + m, 0x24), (0x83 + m, 0x24),  # sustain level, release
        (0xE0 + m, 0x00), (0xE3 + m, 0x00),  # sine
        (0xC0 + ch, 0x02),                   # feedback 1, FM (clean tone)
        (0xA0 + ch, FNUM[ch] & 0xFF),
        (0xB0 + ch, (0x20 if key else 0) | (4 << 2) | FNUM[ch] >> 8),
    ):
        p.poke(PAGE + reg, val)


def start():
    p = Asm()
    p.xreg(0, 1, 1, PAGE)
    p.poke(PAGE + 0x01, 0x20)  # waveform select enable
    return p


def spin(p):
    p.jmp_abs(p.here())
    return p


def opl0():
    return spin(start())


def opl1():
    p = start()
    voice(p, 0)
    return spin(p)


def opl9():
    p = start()
    for ch in range(9):
        voice(p, ch)
    return spin(p)


def oplrhy():
    p = start()
    for ch in range(6):
        voice(p, ch)
    for ch in range(6, 9):  # drum channels: patch, but keyed by $BD
        voice(p, ch, key=False)
    p.poke(PAGE + 0xBD, 0x3F)  # rhythm mode, BD SD TT CY HH on
    return spin(p)


def oplspam():
    p = start()
    for ch in range(9):
        voice(p, ch)
    # Step 0 holds RW0 on channel 0's f-number low register; each store is
    # an OPL register write the RIA queues for the audio IRQ.
    p.store(RW0_STEP, 0x00)
    p.store(RW0_ADDR, 0xA0)
    p.store(RW0_ADDR + 1, PAGE >> 8)
    loop = p.local("spam")
    p.lda_imm(FNUM[0] & 0xFF)
    p.symbol(loop)
    p.sta_abs(RW0_DATA)
    p.jmp_abs(loop)
    return p


ROMS = {f.__name__: f for f in (opl0, opl1, opl9, oplrhy, oplspam)}

if __name__ == "__main__":
    out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    for name, make in ROMS.items():
        size = make().write(out / f"{name}.rp6502")
        print(f"{out / name}.rp6502  {size} bytes")
