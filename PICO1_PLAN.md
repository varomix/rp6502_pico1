# RP6502 on the Pi Pico 1 (RP2040) — Port Plan

An experimental fork of [picocomputer/rp6502](https://github.com/picocomputer/rp6502)
that aims to run as close to the full upstream firmware as possible on the
original Raspberry Pi Pico / Pico W (RP2040). Upstream is RP2350-only and will
not take these changes; this fork carries them.

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

Keep **one source tree** that still builds for RP2350. All RP2040 changes are
behind `PICO_RP2040` (set by the SDK from `PICO_BOARD`). Never break the
upstream build — that keeps merging upstream practical.

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
- [ ] VGA modes 1, 2, 3, 5 (mode 4 done); 720p; SXGA — check the clock
      table still locks for those timings now that 480p uses 252 MHz
- [x] RIA boots to monitor over the VGA console; `STATUS`
- [x] 6502 runs at 8 MHz (sys 256 MHz, 1.20 V) — Klaus functional tests
      via `tools/pico1/klaus_rom.py`: 6502 test 49 passes / 10 min soak,
      65C02 extended 17 passes, 0 failures, 0 VGA scanline misses
- [ ] USB keyboard/mouse/gamepad (MSC works: FLASH/LOAD from the stick)
- [x] `LOAD` a ROM
- [ ] Audio: PSG, OPL (watch for core-0 overload / crackle), bell
- [x] VSYNC (backchannel trap) arrives; [ ] confirm 60 Hz in a game
- [ ] WiFi join, NTP, telnet modem; BLE keyboard
- [ ] Save settings (flash write) while a program runs and video is active
- [x] `FLASH` update of RIA and VGA from MSC

### Before release
- Gate or remove debug: VGA per-page FLASH lines, missed-scanline prints,
  capture commands (`sys/cap.c`), backchannel counters in `STATUS`,
  `PICO1_BC_SPLIT`.

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
