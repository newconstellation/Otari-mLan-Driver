#!/bin/bash
# Removes ND20Audio.driver and restarts Core Audio (asks for your Mac password).
sudo rm -rf /Library/Audio/Plug-Ins/HAL/ND20Audio.driver
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || sudo killall coreaudiod
echo "Removed ND20Audio.driver."
