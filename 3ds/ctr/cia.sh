#!/bin/sh
# Packs ctr/gba-ds.cia from what `make cia-parts` left in build/: run in this folder by `make cia` in ../
# (in an ubuntu:24.04 container: makerom's release binaries want a newer glibc than the devkitARM image has).
#   cia.sh <makerom> <version: major.minor[.micro]>    (BUILD, TARGET: as for make; build, gba-ds)
set -eu
MAKEROM="${1:-makerom}"
V="${2:-0.0.0}"
V="${V%%-*}" # (0.0.1-alpha: the numbers)
MAJOR=$(echo "$V" | cut -d. -f1)
MINOR=$(echo "$V" | cut -d. -f2)
MICRO=$(echo "$V" | cut -d. -f3)
B="${BUILD:-build}"
T="${TARGET:-gba-ds}"
"$MAKEROM" -f cia -o "$T.cia" -elf "$B/$T.elf" -rsf "$B/gba-ds.rsf" -target t -exefslogo \
    -icon "$B/icon.icn" -banner "$B/banner.bnr" -major "${MAJOR:-0}" -minor "${MINOR:-0}" -micro "${MICRO:-0}"
echo "built $T.cia"
