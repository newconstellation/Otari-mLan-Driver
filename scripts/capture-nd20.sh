#!/bin/bash
# Captures what macOS sees on the FireWire bus (incl. the ND-20's config ROM).
# Usage: connect + power on the ND-20, then run:  bash capture-nd20.sh
OUT="$HOME/Desktop/nd20.txt"
{
  echo "== sw_vers"; sw_vers
  echo "== IOFireWireDevice"; ioreg -l -w0 -r -c IOFireWireDevice
  echo "== IOFireWireUnit";   ioreg -l -w0 -r -c IOFireWireUnit
} > "$OUT"
echo "Saved to $OUT"
