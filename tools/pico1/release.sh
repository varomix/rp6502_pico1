#!/bin/sh
# Pico 1 port: build the release images from a clean tree.
#   tools/pico1/release.sh <version>      e.g. tools/pico1/release.sh 0.1
# Writes build/release/rp6502-pico1-<version>-{ria,ria-w,vga}.uf2 and
# SHA256SUMS. ria is for a plain Pico, ria-w for a Pico W; vga runs on
# either. Debug aids (RP6502_PICO1_DEBUG) are off.
set -e
V=${1:?usage: release.sh <version>}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
if [ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]; then
    echo "release.sh: commit or stash your changes first" >&2
    exit 1
fi
OUT=$ROOT/build/release
rm -rf "$OUT"
mkdir -p "$OUT"
for BOARD in pico pico_w; do
    RP6502_VERSION=$V PICO1_BUILD_DIR=$OUT/$BOARD \
        "$ROOT/tools/pico1/build.sh" $BOARD Release
done
cp "$OUT/pico/ria/rp6502-ria.uf2" "$OUT/rp6502-pico1-$V-ria.uf2"
cp "$OUT/pico_w/ria-w/rp6502-ria-w.uf2" "$OUT/rp6502-pico1-$V-ria-w.uf2"
cp "$OUT/pico/vga/rp6502-vga.uf2" "$OUT/rp6502-pico1-$V-vga.uf2"
cd "$OUT"
shasum -a 256 rp6502-pico1-$V-*.uf2 > SHA256SUMS
cat SHA256SUMS
