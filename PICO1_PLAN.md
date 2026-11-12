# RP6502 on the Pi Pico 1 (RP2040)

This repository is the Pico 1 port of
[picocomputer/rp6502](https://github.com/picocomputer/rp6502): the full
firmware, as close to upstream as the hardware allows, on the original
Raspberry Pi Pico / Pico W (RP2040), and only there. Upstream is RP2350-only
and will not take these changes, so this fork carries them on `main`. It is
not a Pico 2 project: keeping the RP2350 build working or staying mergeable
with upstream is not a goal.

This file is the port's plan, status, hardware checklist and log.

Baseline: upstream `ad6293c` (v0.42+, 2026-10-26).
Target hardware: Pico W as RIA (RIA-W), Pico or Pico W as VGA.

---

## 1. RP2350 vs RP2040 — what actually matters

| Resource | RP2350 (Pico 2) | RP2040 (Pico 1) | Impact |
|---|---|---|---|
| CPU | 2× Cortex-M33 @150 MHz, FPU, DSP | 2× Cortex-M0+ @133 MHz, no FPU, no DSP | Audio synth (OPL) and VGA rendering are slower per clock |
| SRAM | 520 KB | 264 KB | Firmware is built `copy_to_ram`; will not fit |
| PIO blocks | 3 (12 SMs) | 2 (8 SMs) | RIA uses ~all of pio0+pio1; CYW43 WiFi needs a third |
| DMA channels | 16 | 12 | OK — VGA uses 7, RIA uses 4 |
| Clock target | 256 MHz (RIA), up to 216 MHz (VGA) | 133 MHz rated, 250–270 MHz typical OC | 6502 PHI2 = sys/32, so 8 MHz needs 256 MHz |
| UF2 family | RP2350_ARM_S | RP2040 | Monitor `FLASH` command checks family |
| USB host | STOP_EPX_ON_NAK hw feature | 2-SOF fallback | Already handled in `vendor/tinyusb_rp6502/hcd_rp2040.c` |
| Pins | Pico 2 W wireless on GPIO 23/24/25/29 | Pico W identical | Reference PCB pinout works unchanged |

### PIO budget today (RIA)

| Block | Programs | Instr / 32 | SMs / 4 |
|---|---|---|---|
| pio0 | ria_cs_rwb 8, ria_write 13, ria_read 5, vga_vsync_trap 4 | 30 | 4 |
| pio1 | ria_action 23, pix_tx 5, vga_backchannel_rx 4 | 32 | 3 |
| pio2 (RP2350 only) | CYW43 gSPI (SDK) | — | 1 |

VGA side: scanvideo (composable 16 + htiming 6 + states 4) and pix_rx 14 +
ria_backchannel_tx 5 — fits two blocks comfortably.

---

## 2. Strategy

The port started by keeping RP2040 changes behind `PICO_RP2040` so the
RP2350 build still worked. That is no longer a goal (2026-11-08): this fork
is for the Pico 1 only, so changes are made for the RP2040 directly. The
existing guards stay; the RP2350 build may break and nobody needs to fix it.
Upstream's host test suite is the parity check: the emulator builds the VGA
modes and emu8950 with their RP2040 paths and must pass it.

Order of work: build plumbing → VGA → RIA (no radio) → RIA-W → polish.
Each phase ends with something flashable and tested on real hardware.

---

## 3. Phases

### Phase 0 — Toolchain & baseline  ✅
- [x] ARM GNU 15.2.Rel1 + Pico SDK 2.3.0 in `~/.pico-sdk` (matches CMakeLists).
- [x] Build upstream for `pico2_w` unmodified: RIA-W 478.5 KB RAM, VGA 379.5 KB RAM.
- [x] `tools/pico1/build.sh <board>` builds + runs the flash-safety check.

### Phase 1 — Build plumbing (compiles for `pico` / `pico_w`)  ✅
- [x] `src/host/pico/CMakeLists.txt`: accept `pico` → `ria`, `pico_w` → `ria-w`.
- [x] `sys/rp2350.{c,h}`: introduce `SYS_KHZ` per chip
      (RP2350: 256000 @1.15 V; RP2040: 256000 @1.20–1.30 V, tunable).
      Keep `SYS_RP2350_KHZ` as an alias so upstream merges stay small, or
      rename everywhere (`phi2.c`, `aud/mix.c`, `ria-w/net/cyw.c`).
- [x] RP2040 flash clock: at 256 MHz the QSPI must be divided by ≥3
      (`PICO_FLASH_SPI_CLKDIV=4`) so `__in_flash` code and XIP stay reliable.
- [x] `ria/sys/ria.c:721`: guard `pio2->input_sync_bypass` with `NUM_PIOS > 2`.
- [x] `ria/mon/uf2.c:98`: accept `RP2040_FAMILY_ID` on RP2040 builds.
- [x] Fix any other RP2350-only SDK symbols the compiler finds.
- [x] Outcome: all three targets compile; record how far over RAM they are.

### Phase 2 — VGA on RP2040
Least risk; VGA worked on RP2040 up to upstream v0.9.
- [ ] Fit RAM (64 KB XRAM mirror + scanline buffers + code). If `copy_to_ram`
      overflows, switch to the default flash binary and mark the render
      path `__not_in_flash_func` / `.time_critical`.
      **Caveat:** `vga/sys/flash.c` erases flash while core 1 renders — core 1
      code and data must be 100 % in RAM. Audit with the map file.
- [ ] Verify sys clocks 201.6 / 216 / 148.5 MHz lock on RP2040 (vreg 1.20 V).
- [ ] Test each mode (0 console, 1 tile, 2 char, 3 bitmap, 4 sprite,
      5 affine) at 640×480 and 1280×720; profile scanline render time on
      the M0+. Use the RP2040 interpolators (already linked) for hot loops.
- [ ] Document which modes/resolutions don't keep up; degrade gracefully.

### Phase 3 — RIA (no radio) on RP2040
- [ ] RAM: move from `copy_to_ram` to flash-resident build with only the
      hot paths in RAM:
      - core 1 `act_loop` + everything it calls (6502 bus actions),
      - IRQ handlers (PIO, DMA, PWM audio, USB),
      - audio mixer inner loops.
      Everything that runs while flash is being written (`PICO_FLASH_ASSUME_CORE1_SAFE`)
      must be RAM-resident. Verify with `arm-none-eabi-nm` + map.
- [ ] Use `tests/fwsize.py` to find the biggest RAM consumers; trim buffers
      that were sized for 520 KB.
- [ ] Clock: try 256 MHz at 1.20 V → 1.25 V → 1.30 V. If a given chip
      won't hold 256 MHz, fall back to 240/200 MHz and lower `PHI2_MAX_KHZ`
      (sys/32) — 6502 then tops out at 7.5 / 6.25 MHz.
- [ ] PIO: pio0+pio1 as today (no change needed without radio).
- [ ] Audio: `emu8950` OPL on M0+ (no FPU/DSP) is the CPU risk. Measure
      core-0 load. Options if too slow: lower OPL internal rate, fixed-point
      hot loops, or make OPL an optional build flag.
- [ ] USB host keyboard/mouse/gamepad/MSC — validate the RP2040 SOF path.
- [ ] Test: boot, monitor, `LOAD` ROMs, 6502 functional test at 8 MHz.

### Phase 4 — RIA-W (WiFi / BLE / modem / NTP) on Pico W
The hard part: the CYW43 gSPI driver needs one PIO SM + ~10 instructions,
and pio0/pio1 have 2 and 0 instructions free respectively.
Options, in order of preference:
1. **Repack PIO programs** — the 4-instr `vga_vsync_trap` and
   `vga_backchannel_rx` are small; relocate/merge so one block has
   ≥ CYW43 program length free + one SM. E.g. move `pix_tx` (5) or
   `vga_backchannel_rx` (4) into pio0's spare 2 slots if trimmed, or fold
   vsync-trap into the backchannel SM.
2. **Shrink `ria_action`** (23 instr) — review for redundancy.
3. **Time-share** — CYW43 transactions are bursty; loading its program on
   demand is fragile, last resort.
4. **Bit-banged gSPI on core 0** — no PIO needed, slower WiFi; fallback.
- [ ] RAM: lwIP + BTstack + CYW43 firmware blob (in flash via
      `CYW43_RESOURCE_ATTRIBUTE __in_flash`) — expect to cut pbuf pools,
      maybe make BLE optional (`RP6502_BLE=0`).
- [ ] `ria-w/net/cyw.c` PIO clock divider uses `SYS_RP2350_KHZ` — re-tune.
- [ ] Test: WiFi join, NTP, telnet modem, BLE keyboard.

### Phase 5 — Polish & release
- [ ] Release script producing `rp6502-pico1-<ver>-{ria,ria-w,vga}.uf2`.
- [ ] README section: what works, measured max PHI2, known limits.
- [ ] Upstream sync procedure (rebase onto picocomputer/rp6502 tags).

---

## 4. Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| RIA-W won't fit 264 KB even flash-resident | Medium | Make BLE / OPL optional build flags |
| No PIO room for CYW43 | High | Repack (Phase 4 opt. 1), else bit-bang |
| Chip won't overclock to 256 MHz | Medium (per-chip) | Lower PHI2 max; configurable sys clock |
| M0+ too slow for OPL synthesis | Medium | Reduced rate / optional OPL |
| M0+ too slow for some VGA modes at 720p | Medium | Document; limit to 480p for heavy modes |
| Flash writes while core 1 executes from XIP | High if missed | Map-file audit; RAM placement |

---

## 5. Status (2026-11-08)

All of Phases 1–4 are **implemented and building**. RIA-W + VGA boot on real
Pico 1 W / Pico 1 hardware, load ROMs, and update each other with `FLASH`.
Remaining work is the unchecked hardware items below, then Phase 5.

### Verified on hardware (2026-11-06)
- VGA 640×480 at **252 MHz / 1.25 V** (10× pixel clock). At 201.6 MHz the
  M0+ missed scanlines (blue lines, sprite holes).
- Mode 4 sprites after the M0+ unaligned-load fix (byte-safe metadata,
  odd image pointers skipped).
- RIA ↔ VGA backchannel: VSYNC, PIX ACK/NAK (after `cs_rwb` went to 9 instrs
  so the 5-instr rx_trap fits pio0).
- `LOAD` of a ROM (Flappy Coo); VGA XRAM matches the ROM on every load
  after DMA data channels are started through the trigger alias.
- `FLASH MSC0:/…uf2` updates the RIA and, through it, the VGA (staged in
  spare flash, copied by a bootrom-only RAM routine). No BOOTSEL needed.
- Debug aids: `tools/pico1/{console,upload,vgacap}.py`, scanline capture,
  missed-scanline / render-time prints on the VGA USB console.

### How RAM was made to fit
| | RP2350 build | RP2040 build | free |
|---|---|---|---|
| RIA-W | 478.5 KB | ~210 KB | ~54 KB |
| RIA | — | ~139 KB | ~125 KB |
| VGA | 379.5 KB | ~259 KB | ~5 KB |

- Not `copy_to_ram` on RP2040. Hot code goes to RAM **by object file name**:
  SDK linker-script overrides (`src/host/pico/{ria,vga}/ld_rp2040/`) exclude
  listed objects from flash `.text`/`.rodata`, and those objects are built
  `-fno-lto` so their names survive the link. Lists live in `ria.cmake`
  (`RP6502_RIA_RAM_SOURCES`) and `vga/CMakeLists.txt` (`RP6502_VGA_RAM_SOURCES`)
  — keep them in step with the `.incl` patterns.
- A few single functions are `HOST_NOT_IN_FLASH`: `term_view`,
  `term_view_row`, `sys_stop`, `proc_exit`.
- VGA: xram placed first in RAM (kills ~56 KB alignment padding); compact
  `vga_prog` rows with 1-byte renderer ids (22.5→12 KB); 8 scanline buffers
  (was 10); `font8`/`italic16` in spare scratch X/Y halves.
- `tools/pico1/hotpath.py` walks the call graph (incl. literal-pool
  references to code *and* const data) from core-1 / render / IRQ roots and
  lists anything still in flash. Core 1 and VGA render: **0 bytes in flash**.

### How the PIO budget was made to fit (RIA-W)
| Block | Programs | Instr | SMs |
|---|---|---|---|
| pio0 | cs_rwb 9, write 13, read 5, **rx_trap 5** | 32/32 | 4/4 |
| pio1 | **action_fixed 21**, pix_tx 5, CYW43 gSPI 6 | 32/32 | 3/4 |

- `ria_cs_rwb`: PIO v0 has no `mov pindirs` → `out pindirs 8` from OSR (+1
  instr, +1 cycle ≈ 4 ns; shared with RP2350).
- `vga_backchannel_rx_trap`: receiver raises IRQ flag 0 on each framed start
  bit; the trap is that flag, masked except while armed (replaces the
  separate `vga_vsync_trap` SM).
- `ria_action_fixed`: variable address loaded into X once at init instead
  of `pull noblock; mov x osr` every cycle (the RIA sets it only once).
- RIA now `pio_sm_claim`s its fixed SMs — without this the CYW43 driver would
  have claimed pio1 SM0 (the 6502 action SM).

### Clocks
- RIA 256 MHz @ 1.20 V (`ria/sys/rp2350.c`); QSPI flash clkdiv 4 (64 MHz).
- VGA 148.5–216 MHz @ 1.20 V (set before reclock in `vga/sys/vga.c`).

### Flashing (Pico 1 / Pico 1 W)
- RIA on Pico 1 W: `build/pico_w/ria-w/rp6502-ria-w.uf2`
  (fallback without radio: `build/pico/ria/rp6502-ria.uf2`)
- VGA on any Pico 1: `build/pico/vga/rp6502-vga.uf2`

### Hardware test checklist
- [x] VGA boots, console visible at 640×480 (252 MHz)
- [x] A real game: ogodfrey/space-invaders-rp6502 (mode 3 320×240 4bpp
      bitmap + console plane + PSG) plays correctly at 640×480, no scanline
      misses, PSG 22% of core 0. Built with cl65 -t rp6502 + rp6502.py create.
- [x] A heavy game: rumbledethumps/quarry (prebuilt ROM; modes 2, 3, 5 on
      three planes, ~200 custom sprites) plays with 0 scanline misses after:
      mode 5 custom-sprite scan out of line (13-instr reject), 2 bpp custom
      sprites a byte at a time, and `mode_word_t` aligned(2) on RP2040 (GCC
      was storing each pixel pair as 4 bytes: 324 strb sequences gone from
      modes 1/2/3/5). Worst gameplay line 142–144 µs (~139 µs per core per
      line at 320×240): thin margin.
- [x] VGA test corpus (tests/cpu/vga/vidmodes.py, `tools/pico1/vgarun.py`)
      at 640×480: 105/111 clean before the speedups; the 6 that miss are
      deliberate overload/stacking tests. Found and fixed: mode 3 16 bpp at
      an odd XRAM address hard-faulted the M0+.
- [x] Corpus re-run after the speedups, all three display types; clocks
      lock and nothing crashes. Clean: 640×480 105/111, 16:9 (720p,
      148.5 MHz) 103/111, SXGA (216 MHz) 99/111. Misses are the deliberate
      overload tests (sprite_overrun/pair, mode5c_overrun) and full-width
      stacking (text_three640, mode3_16parity, fill_three640*); SXGA also
      drops mode5c_pair/stack and mode0_win360. SXGA has the least headroom.
- [x] Odd-address mode 4 sprites draw (were skipped on RP2040); mode4_odd and
      mode4a_odd clean on hardware.
- [x] RIA boots to monitor over the VGA console; `STATUS`
- [x] 6502 runs at 8 MHz (sys 256 MHz, 1.20 V) — Klaus functional tests
      via `tools/pico1/klaus_rom.py`: 6502 test 49 passes / 10 min soak,
      65C02 extended 17 passes, 0 failures, 0 VGA scanline misses
- [x] USB gamepad: every d-pad/stick direction and button reads correctly
      (tests/cpu/hid/gamepad.py tester); MSC works (FLASH/LOAD from the stick)
- [x] USB keyboard/mouse input (`tools/pico1/hid_rom.py`): typing incl.
      Enter, Backspace and shifted symbols, mouse movement, buttons and wheel
      all correct on screen (2026-11-12, checked by eye). Backspace sends
      DEL (0x7F) like a VT100, so a program must echo BS SP BS; the first
      ROM echoed DEL raw and nothing was erased.
- [x] BLE gamepad: Xbox controller pairs (`SET BLE 2`) and every input reads,
      alongside the USB pad as P1. First end-to-end proof of the CYW43 on the
      repacked PIO. Its button labels report GAMEPAD_TYPE_UNKNOWN.
- [x] `LOAD` a ROM
- [x] Audio OPL: audio IRQ load on core 0 measured with `tools/pico1/opl_rom.py`
      and the `[AUD …]` debug line. emu8950 skips silent operators
      (`EMU8950_SKIP_SILENT`, sample-exact vs. the full path) and force-inlines
      the channel helpers; IRQ drains ≤4 writes/sample; an overrun makes the
      next IRQ hold the level (no more core-0 lockups — `oplspam` used to hang
      the RIA). Idle 43%, 1 voice 48%, rhythm 75%, 9 voices 84% (~1 held
      sample/s). `-O2` was slower than `-Os` here. Closed 2026-11-12 by
      RPMegaChopperII (below): OPL music at 82% with USB pads and WiFi up
      sounds clean.
- [x] Audio PSG (`tools/pico1/psg_rom.py`): idle 21%, 8-voice chord 24%,
      all 8 frequencies rewritten nonstop 46%; no overruns; sounds clean
- [x] Audio: bell rings on BEL (Ctrl+G echoed by `hid_rom.py`)
- [x] VSYNC (backchannel trap) arrives; [ ] confirm 60 Hz in a game
- [x] WiFi join (-70 dBm, 2.4 GHz), NTP (time + TZ correct), remote console
      over telnet (`SET PORT`/`SET KEY`, passkey, STATUS/HELP), modem
      (`tools/pico1/modem_rom.py` bridges console <-> AT:): AT/OK, `ATD
      host:port` CONNECT, 200 typed bytes and a 3.4 KB block from the host
      both intact, `+++` escape, `ATH`. 0 VGA scanline misses throughout.
      A console paste at full 115200 outruns a 1-byte-per-segment writer and
      overflows the 32-byte RIA UART ring (same size upstream).
- [x] Audio IRQ overruns during WiFi activity (1 -> 7 per modem session, none
      with ping floods, telnet, flash writes or console input). Cause: the
      timer IRQ handlers ran from flash. The CYW43 send path sleeps on the
      SDK alarm pool, and `alarm_pool_irq_handler` / the VGA vsync
      `hardware_alarm_irq_handler` (priority 0x40, above audio) then
      cache-missed behind lwIP/CYW43 code for 12-16 us. Found with a PC log
      on late audio entries (`AUDlate` in STATUS) and an enabled-IRQ dump
      (`IRQ` lines). `hardware_timer/timer.c` and `pico_time/time.c` now
      live in RAM: modem send 92 late / 7 over -> 2 late / 0 over, full
      modem session max 5137 -> 2056 clk. The 2 left are `get_rand_64`
      (pico_rand masks IRQs for ROSC samples, once per TCP connect, ~4400
      clk, inside the period).
      Note: on the M0+ only the top two priority bits exist, so audio's
      `DEFAULT + 0x10` equals the default level, not below it.
- [x] RPMegaChopperII (jasonfrowe, v0.1.0 release ROM: three planes, mode 5
      sprites, OPL2 VGM music): 0 VGA scanline misses at 640x480, worst
      line ~163 us.
- [x] OPL load in RPMegaChopperII: 82-83% of core 0 per sample, ~50-60 held
      samples/s (`over` 100-160 per 2 s), same with RF off, so it is the
      synth itself, not WiFi. Heavier than the 9-voice synthetic test
      (~1/s). Listening test 2026-11-12: game plays and sounds fine, no
      audible artifacts, so no further emu8950 work for now.
- [ ] BLE keyboard (BLE gamepad done)
- [x] Save settings (flash write) while a program runs and video is active
      (`tools/pico1/cfgsave_rom.py`): a running program can only write flash
      through the modem (`AT0:` then `AT&W`/`AT&Z`; plain `AT:` has no slot,
      so `AT&W` gives ERROR) or BLE bonding. 88 saves in 30 s over an 8-voice
      PSG chord: 6502 never stalls (VSYNC +20 frames per save), 0 new VGA
      misses, USB and WiFi fine. Audio: `lfs_prog`/`lfs_erase` run with
      core 0's IRQs off, so each page program holds 1-4 samples and each
      sector erase (~every 4 s under this load) drops ~1500 samples, about
      30 ms. Known limitation; fix would keep RAM-only IRQs (audio, VSYNC,
      backchannel) enabled during flash ops.
- [x] `FLASH` update of RIA and VGA from MSC

### Before release
- Gate or remove debug: VGA per-page FLASH lines, missed-scanline prints,
  capture commands (`sys/cap.c`), backchannel counters in `STATUS`,
  `PICO1_BC_SPLIT`, the `[AUD …]` line / `aud_status_response` /
  `aud_dbg_task`, the `AUDlate` / `IRQ` STATUS lines and `aud_irq_entry`
  trampoline, `EMU8950_PROF` section timing, and the VGA
  `[vgaw …]` worst-line per-plane timing (and com_dbg_out's bool).

## 6. Log

- 2026-11-06 — Plan written. Fork cloned, toolchain installed.
- 2026-11-06 — Phase 1 plumbing; RAM fit for RIA/RIA-W/VGA; core-1 and
  render paths verified flash-free; PIO repack for CYW43. All RP2040
  targets build; RP2350 still builds. Awaiting hardware test.
- 2026-11-06 — First hardware runs. Fixed: pio0 overflow (backchannel dead),
  VGA scanline misses (→ 252 MHz), M0+ unaligned sprite loads, RP2040 DMA
  CHAIN_TO stale-address race. Added XIP-safe staged `FLASH` for RIA and VGA.
- 2026-11-08 — Plan status refreshed. 8 MHz 6502 verified: Klaus 6502 and
  65C02 functional tests pass continuously at 1.20 V (10 min soak, 0 errors).
  `upload.py` waits out the monitor's terminal probe after the prompt.
- 2026-11-12 — WiFi, NTP, telnet console and modem verified. Audio IRQ
  overruns under WiFi traced to flash-resident timer IRQs; moved to RAM.
  RPMegaChopperII runs clean on VGA; its OPL music overloads core 0.
- 2026-11-12 — USB keyboard, mouse and bell verified by eye; flash saves
  during a running program are safe apart from ~30 ms audio gaps per erase.
