It's always worth the tokens to read ~/picocomputer.github.io at the start
of every session.

This repository is the Pico 1 port of the RP6502: it runs the firmware on the
original Raspberry Pi Pico / Pico W (RP2040), and only there. Upstream
(picocomputer/rp6502) is RP2350-only and does not take these changes; this
fork carries them, on main. It is not a Pico 2 project: there is no need to
keep the RP2350 build working or to stay mergeable with upstream, so make
changes for the RP2040 directly rather than adding compatibility for the
Pico 2. Existing #if PICO_RP2040 guards are harmless and can stay.

Build and check with tools/pico1/build.sh pico_w (or pico). The emulator and
upstream's test suite (src/host/libretro, RP6502_TESTS=ON, ctest) build the
VGA modes and emu8950 with their RP2040 paths, so the tests check what the
Pico 1 runs. PICO1_PLAN.md holds the port's status, hardware checklist and
log; keep it current.

The RP2040 has 264 KB of RAM, a Cortex-M0+ with no FPU and no unaligned
access, and two PIO blocks. Code that core 1, the VGA render or an IRQ can
reach must stay in RAM; tools/pico1/hotpath.py checks it.

A machine is everything that comes together to make a Picocomputer.
src/host brings together the core with the needed services.
src/osal connects the core with an operating system.
src/core is the bulk of what makes a Picocomputer.

We do not use the TinyUSB in the Pi Pico SDK.
We have overrides for many of our submodules. See: vendor/*_rp6502

We do not waste memory defending against bad code. Every byte is precious but
it's always better to fix bad code even if it's a few more bytes than a hack.
Prefer improving the architecture over a wrapper or other hack.

Do not commit or push unless specifically asked to. Do not look for answers
in git history unless asked to.
