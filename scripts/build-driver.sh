#!/bin/bash
# Builds ND20Audio.driver (the Core Audio plug-in).
# Optional:  bash scripts/build-driver.sh 64 32 0,1
#            (inputs, outputs, ND-20 routing numbers to receive; defaults 64 32 0,1)
set -e
cd "$(dirname "$0")/.."
IN=${1:-64}; OUT=${2:-32}; RX=${3:-0,1}
B=build/ND20Audio.driver/Contents
rm -rf build/ND20Audio.driver
mkdir -p "$B/MacOS"
cp src/ND20Audio/Info.plist "$B/Info.plist"
clang -arch x86_64 -mmacosx-version-min=10.14 -O2 -Wall -bundle \
  -DND20_INPUTS=$IN -DND20_OUTPUTS=$OUT -DND20_RX_CHANNELS=$RX \
  -framework CoreAudio -framework CoreFoundation -framework IOKit \
  -o "$B/MacOS/ND20Audio" src/ND20Audio/ND20Audio.c src/nd20rx/nd20rx.c
codesign --force --sign - build/ND20Audio.driver 2>/dev/null || echo "(codesign skipped)"
echo "Built build/ND20Audio.driver with $IN inputs and $OUT outputs, receiving routing $RX"
