#!/bin/bash
# Builds the read-only ND-20 probe. Needs Xcode Command Line Tools (run: xcode-select --install).
set -e
cd "$(dirname "$0")/.."
mkdir -p build
clang -arch x86_64 -mmacosx-version-min=10.14 -Wall -O1 \
  -framework IOKit -framework CoreFoundation \
  -o build/nd20probe src/nd20probe/nd20probe.c
echo "Built build/nd20probe"
