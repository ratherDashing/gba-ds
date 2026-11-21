#!/bin/bash
# libmgba for the 3DS: the commit and options every GBA DS build uses (scripts/mgba-options.sh), with
# mGBA's own 3DS port's toolchain. Run inside the devkitpro/devkitarm image (../Makefile's `libmgba`).
#
#   build-libmgba.sh [DEPS] [color16]    DEPS/mgba-3ds/build/libmgba.a (DEPS: the repo's deps/ by default)
# color16: mGBA's own 3DS port's 16-bit pixels (RGB565) in place of 32-bit ones (GBA DS builds with COLOR_16_BIT then)
# MGBA_BUILD_NAME=NAME builds in DEPS/mgba-3ds/NAME instead of build/ (the sources are shared); MGBA_CFLAGS adds C flags
# after the release flags; MGBA_LTO=1 builds libmgba with -flto (the app's own link then optimizes across the library).
# MGBA_PGO=use builds with the profile in pgo/ (made from a run of -fprofile-generate code: MGBA_PGO=gen, whose app
# links -lgcov and writes its .gcda files to /gcov on the SD card at exit; copy them into pgo/ as they are named).
# The profile fits only the libmgba sources and patches it was made from (the last line says how many functions it misses):
# retrain after changing them, on advance-wars, golden-sun, super-mario-advance, tony-hawks-pro-skater-2, wario-land-4,
# f-zero-maximum-velocity, mario-and-luigi-superstar-saga and sonic-advance-2 (from power-on, 3000 to 5000 frames each).
# rom-copy-size.patch: a copy of a ROM that a copy of the game writes to (patches, a cartridge clock or
# rumble) is the ROM's size, not 32 MiB. memory-map-hook.patch: mGBA's anonymousMemoryMap is GBA DS's
# (3ds/ctr/emu_ctr.c), so those copies come from the top of the heap's range, as the ROMs do.
# video-thread-core.patch: the core of mGBA's drawing thread is gbadsVideoCore (3ds/ctr/host.c), core 2 unless
# video_core in gba-ds.cfg says another.
# gba-fast-memory.patch: short paths for the usual loads, stores and jumps (RAM and ROM, no idle loop to check) in
# front of mGBA's own code, which stays as the slow path: same results, fewer instructions.
# gba-ldm-stm.patch: LDM and STM (and so PUSH and POP) go through the set bits of the register list, not all sixteen.
# gba-dma-batch.patch: a DMA transfer's next unit runs straight from the one before it when nothing else is due first (the
# steps the event loop took, without the list work); mTimingTick reads the master clock again after each callback.
# color-mix.patch: mColorMix5Bit (the renderer's alpha blend, per pixel) with two multiplies fewer: the same value for every
# input (checked on all channel pairs and weights up to 16).
# video-wake.patch: how often the drawing thread is woken (every 16 lines as before; video_wake in gba-ds.cfg).
# gba-stall-closed-form.patch: the ROM prefetch model's loop (GBAMemoryStall) as arithmetic and a small table, with the
# result unchanged.
# gba-jump-inline.patch: the usual jump (same region, no idle loop) is checked inside the instruction that jumps, no call.
# gba-audio-fast.patch: the sound path without the noise channel's tables copied onto the stack on every call, or a
# memset and a division per event (the 3DS's CPU has no divide); the same samples.
# gba-audio-silent.patch: the sampling loop makes the PSG's output once per run of equal channel samples and skips the PSG's
# run when no channel is playing.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT="$PWD"
DEPS="${1:-$ROOT/deps}"
SRC="$DEPS/mgba-3ds"
. scripts/mgba-options.sh

if [ ! -d "$SRC/.git" ]; then
	git clone -q https://github.com/mgba-emu/mgba.git "$SRC"
fi
git -C "$SRC" checkout -q -f "$MGBA_COMMIT"
# GBA DS runs several copies of a game at once: not mGBA's one fixed ROM buffer, nor the 32 MiB that buffer's
# start-up code takes. Its pixels are 32-bit, unless color16 asks for mGBA's own 16-bit ones.
if [ "${2:-}" = color16 ]; then
	sed -i -e 's/ FIXED_ROM_BUFFER / /' -e '/ctru-heap.c/d' "$SRC/src/platform/3ds/CMakeLists.txt"
	grep -q 'COLOR_5_6_5' "$SRC/src/platform/3ds/CMakeLists.txt" || { echo "mGBA's 16-bit defines are gone"; exit 1; }
else
	sed -i -e 's/ COLOR_16_BIT COLOR_5_6_5 FIXED_ROM_BUFFER / /' -e '/ctru-heap.c/d' "$SRC/src/platform/3ds/CMakeLists.txt"
	grep -q 'COLOR_16_BIT' "$SRC/src/platform/3ds/CMakeLists.txt" && { echo "mGBA's 3DS defines not removed"; exit 1; }
fi
git -C "$SRC" apply "$ROOT/3ds/mgba/rom-copy-size.patch" "$ROOT/3ds/mgba/memory-map-hook.patch" \
	"$ROOT/3ds/mgba/video-thread-core.patch" "$ROOT/3ds/mgba/gba-fast-memory.patch" \
	"$ROOT/3ds/mgba/gba-ldm-stm.patch" "$ROOT/3ds/mgba/gba-dma-batch.patch" "$ROOT/3ds/mgba/color-mix.patch" \
	"$ROOT/3ds/mgba/video-wake.patch" "$ROOT/3ds/mgba/gba-stall-closed-form.patch" "$ROOT/3ds/mgba/gba-jump-inline.patch" "$ROOT/3ds/mgba/gba-audio-fast.patch" "$ROOT/3ds/mgba/gba-audio-silent.patch"

OUT="$SRC/${MGBA_BUILD_NAME:-build}"
PGO_FLAGS=
CMAKE_SRC="$SRC"
case "${MGBA_PGO:-}" in
gen) PGO_FLAGS="-fprofile-generate -fprofile-update=single -fprofile-dir=/gcov -fprofile-prefix-path=$OUT" ;;
use) PGO_FLAGS="-fprofile-use=$ROOT/3ds/mgba/pgo -fprofile-prefix-path=$OUT -fprofile-partial-training -fprofile-correction -Wno-error=coverage-mismatch" ;;
esac
# GCC matches a profile to functions by their source path, so PGO builds compile from this one path whatever DEPS is.
if [ -n "${MGBA_PGO:-}" ]; then
	CMAKE_SRC=/tmp/gbads-libmgba-src
	ln -sfn "$SRC" "$CMAKE_SRC"
fi
RELEASE_FLAGS=(-DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG $PGO_FLAGS ${MGBA_CFLAGS:-}")
LTO_OPT=(-DBUILD_LTO=OFF)
[ -n "${MGBA_LTO:-}" ] && LTO_OPT=(-DBUILD_LTO=ON)
# (a build folder made from the other path would make cmake refuse)
grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$CMAKE_SRC" "$OUT/CMakeCache.txt" 2>/dev/null || rm -f "$OUT/CMakeCache.txt"
cmake -S "$CMAKE_SRC" -B "$OUT" -DCMAKE_TOOLCHAIN_FILE="$SRC/src/platform/3ds/CMakeToolchain.txt" \
	"${MGBA_OPTS[@]}" "${LTO_OPT[@]}" "${RELEASE_FLAGS[@]}" -DUSE_LZMA=OFF -DUSE_DEBUGGERS=OFF > "$OUT.log"
make -C "$OUT" -j"${MGBA_JOBS:-$(nproc)}" mgba >> "$OUT.log" 2>&1
# mGBA's own files never include the flags.h it writes for frontends, so the library is built without
# MINIMAL_CORE, which flags.h still claims on consoles: struct mCore's layout must match the library's.
sed -i 's|^#define MINIMAL_CORE ON$|/* (MINIMAL_CORE: the library is not built with it) */|' "$OUT/include/mgba/flags.h"
grep -q '^#define MINIMAL_CORE' "$OUT/include/mgba/flags.h" && { echo "flags.h still says MINIMAL_CORE"; exit 1; }
[ "${MGBA_PGO:-}" = use ] && echo "profile out of date for $(grep -c 'coverage-mismatch' "$OUT.log") warnings (0 is right; else retrain with MGBA_PGO=gen)"
echo "built $OUT/libmgba.a"
