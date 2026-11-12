set(RP6502_PROJECT_DEFINITIONS
    RP6502_NAME="Picocomputer 6502"
    RP6502_LOCALE=EN
    RP6502_EXFAT=0
    RP6502_CREATOR=0
)

# Pico 1 port: -DRP6502_PICO1_DEBUG=ON builds the development aids used by
# tools/pico1 (VGA render timing and missed-scanline lines on the VGA USB
# console, vgacap.py scanline capture, 1200 baud BOOTSEL, backchannel
# registers in STATUS). Release builds leave it off.
option(RP6502_PICO1_DEBUG "Pico 1 port: development aids on the consoles" OFF)
if(RP6502_PICO1_DEBUG)
    list(APPEND RP6502_PROJECT_DEFINITIONS PICO1_DEBUG=1)
endif()

# Force wrappers defined by pico_printf and pico_stdlib to be
# preserved during link-time interprocedural optimizations.
set(IPO_PRINTF_LINK_OPTIONS
    -Wl,-u,__wrap_printf
    -Wl,-u,__wrap_vprintf
    -Wl,-u,__wrap_puts
    -Wl,-u,__wrap_putchar
    -Wl,-u,__wrap_getchar
    -Wl,-u,__wrap_sprintf
    -Wl,-u,__wrap_snprintf
    -Wl,-u,__wrap_vsnprintf
)

include(${RP6502_ROOT}/submodules.cmake)
rp6502_submodule(vendor/littlefs SENTINEL lfs.c
    WANTS "the firmware's flash filesystem")

include(${RP6502_ROOT}/version.cmake)

# If you have thoroughly tested a third-party RP2350 board in the reference
# design then please submit a pull request.
if(PICO_BOARD STREQUAL "pico2_w" OR
   PICO_BOARD STREQUAL "pimoroni_pico_plus2_w_rp2350" OR
   PICO_BOARD STREQUAL "pico_w")
    set(_ria ria-w)
elseif(PICO_BOARD STREQUAL "pico2" OR
       PICO_BOARD STREQUAL "pico")
    set(_ria ria)
else()
    message(FATAL_ERROR "Unknown board: ${PICO_BOARD}")
endif()

add_subdirectory(vga ${CMAKE_BINARY_DIR}/vga)
add_subdirectory(${_ria} ${CMAKE_BINARY_DIR}/${_ria})
