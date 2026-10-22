# Otari ND-20 mLAN Driver for macOS

A from-scratch driver so the **Otari ND-20** (mLAN / IEEE 1394 network audio unit)
works with a **2010 Mac Pro (5,1) running macOS Mojave** and shows up in Pro Tools.

## Approach
Runs entirely in user space, with no kernel extension, so no kext signing and no SIP changes:

1. **FireWire engine:** Apple's IOFireWireLib, which still ships in Mojave. It discovers the ND-20,
   sets up the mLAN connection, and sends and receives isochronous audio
   (IEC 61883-6 / AM824 packets).
2. **Core Audio plug-in:** an AudioServerPlugIn, so the ND-20 appears as a normal
   audio device in Audio MIDI Setup and Pro Tools.

Yamaha's Steinberg FW Driver 1.7.5 is studied only as a reference for the mLAN control
protocol, for interoperability. None of Yamaha's code or files are included here.

## Status
- [x] Unpacked and mapped Yamaha FW Driver 1.7.5 (see `docs/`)
- [ ] Capture the ND-20 config ROM (`scripts/capture-nd20.sh`)
- [ ] Decide control path: mLAN v2 "OGT" registers vs. mLAN v1
- [ ] Step 1 tool: find the ND-20 and read its registers
- [ ] Receive audio from the ND-20
- [ ] Send audio to the ND-20
- [ ] Core Audio plug-in and Pro Tools testing

## Layout
- `docs/`: reverse-engineering notes
- `scripts/`: helpers to run on the Mac Pro
- `tools/re/`: analysis helpers (unpack .pkg installers on any OS)
