#!/bin/bash
# Builds the ND-20 tools. Needs Xcode Command Line Tools (run: xcode-select --install).
set -e
cd "$(dirname "$0")/.."
mkdir -p build
FLAGS="-arch x86_64 -mmacosx-version-min=10.14 -Wall -O1 -framework IOKit -framework CoreFoundation"
clang $FLAGS -o build/nd20probe src/nd20probe/nd20probe.c
echo "Built build/nd20probe"
clang $FLAGS -o build/nd20claim src/nd20claim/nd20claim.c
echo "Built build/nd20claim"
clang $FLAGS -O2 -o build/nd20listen src/nd20listen/nd20listen.c src/nd20rx/nd20rx.c
echo "Built build/nd20listen"
clang $FLAGS -o build/nd20remote src/nd20remote/nd20remote.c
echo "Built build/nd20remote"
