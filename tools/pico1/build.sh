#!/bin/sh
# Pico 1 port: configure and build one board, then check that nothing the
# 6502 bus core / VGA render core / IRQs can reach is left in flash.
#   tools/pico1/build.sh [pico_w|pico|pico2_w|pico2] [Release|Debug]
# PICO1_DEBUG=1 builds the development aids (RP6502_PICO1_DEBUG) that
# vgarun.py and vgacap.py read, into build/<board>_debug.
# PICO1_BUILD_DIR overrides the build directory and RP6502_VERSION stamps a
# version (release.sh sets both).
set -e
BOARD=${1:-pico_w}
TYPE=${2:-Release}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SDK=${PICO_SDK_PATH:-$HOME/.pico-sdk/sdk/2.3.0}
TC=${PICO_TOOLCHAIN_PATH:-$HOME/.pico-sdk/toolchain/15_2_Rel1}
export PICO_SDK_PATH=$SDK PICO_TOOLCHAIN_PATH=$TC PATH=$TC/bin:$PATH
B=${PICO1_BUILD_DIR:-$ROOT/build/$BOARD${PICO1_DEBUG:+_debug}}
cmake -S "$ROOT" -B "$B" -G Ninja -DPICO_BOARD="$BOARD" -DCMAKE_BUILD_TYPE="$TYPE" \
    -DRP6502_PICO1_DEBUG=${PICO1_DEBUG:+ON}${PICO1_DEBUG:-OFF} \
    ${RP6502_VERSION:+-DRP6502_VERSION=$RP6502_VERSION} >/dev/null
ninja -C "$B"
case $BOARD in pico|pico_w) ;; *) exit 0 ;; esac
HP="python3 -I $ROOT/tools/pico1/hotpath.py $TC/bin/arm-none-eabi-objdump $TC/bin/arm-none-eabi-nm"
RIA=$(ls "$B"/ria*/rp6502-ria*.elf)
echo "== core 1 (6502 bus) flash check"
$HP "$RIA" 'act_loop.*' | head -5
echo "== VGA render flash check"
$HP "$B/vga/rp6502-vga.elf" 'vga_render_loop.*' 'mode[0-9]_render.*' 'isr_.*' | head -5
for e in "$RIA" "$B/vga/rp6502-vga.elf"; do
    end=$($TC/bin/arm-none-eabi-nm "$e" | awk '$3=="__HeapLimit"||$3=="end"{print $1}' | sort | tail -1)
    $TC/bin/arm-none-eabi-size "$e" | tail -1
done
ls -la "$B"/*/*.uf2
