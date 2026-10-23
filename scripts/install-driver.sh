#!/bin/bash
# Installs ND20Audio.driver and restarts Core Audio (asks for your Mac password).
set -e
cd "$(dirname "$0")/.."
[ -d build/ND20Audio.driver ] || bash scripts/build-driver.sh
sudo rm -rf /Library/Audio/Plug-Ins/HAL/ND20Audio.driver
sudo cp -R build/ND20Audio.driver /Library/Audio/Plug-Ins/HAL/
sudo chown -R root:wheel /Library/Audio/Plug-Ins/HAL/ND20Audio.driver
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || sudo killall coreaudiod
echo "Installed. Open Audio MIDI Setup and look for \"Otari ND-20 Network\"."
