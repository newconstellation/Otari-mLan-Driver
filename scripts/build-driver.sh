#!/bin/bash
# Builds ND20Audio.driver (the Core Audio plug-in).
# Optional channel counts:  bash scripts/build-driver.sh 64 32     (inputs outputs; default 64 32)
set -e
cd "$(dirname "$0")/.."
IN=${1:-64}; OUT=${2:-32}
B=build/ND20Audio.driver/Contents
rm -rf build/ND20Audio.driver
mkdir -p "$B/MacOS"
cp src/ND20Audio/Info.plist "$B/Info.plist"
clang -arch x86_64 -mmacosx-version-min=10.14 -O2 -Wall -bundle \
  -DND20_INPUTS=$IN -DND20_OUTPUTS=$OUT \
  -framework CoreAudio -framework CoreFoundation \
  -o "$B/MacOS/ND20Audio" src/ND20Audio/ND20Audio.c
codesign --force --sign - build/ND20Audio.driver 2>/dev/null || echo "(codesign skipped)"
echo "Built build/ND20Audio.driver with $IN inputs and $OUT outputs"
