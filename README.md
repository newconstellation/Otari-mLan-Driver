# Otari ND-20 mLAN Driver for macOS

A from-scratch driver so an **Otari ND-20** network (mLAN / IEEE 1394 network audio units)
works with a **2010 Mac Pro (5,1) running macOS Mojave** as a multichannel recording interface in Pro Tools.

Target system: 4 × ND-20 with mic preamps (64 mics), 6 × ND-20 with line outputs, 1 × ND-20 with 16 line inputs,
and a CB-178 remote. The network carries at most 96 channels at 48 kHz, so the realistic goal is
**64 recording inputs plus up to ~32 playback outputs** (see `docs/nd20-manual-notes.md`).

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
- [x] Capture the ND-20 config ROM: it is an **AV/C + IEC 61883** device (see `docs/nd20-config-rom.md`)
- [x] Decide control path: standard AV/C + plug control registers (not Yamaha OGT)
- [x] Read the ND-20's mLAN address map (BootParam / Core / mLAN spaces), see `docs/nd20-config-rom.md`
- [x] Claim the device the way Yamaha's Enabler does (`nd20claim`): the claim works, but registers stay closed on this firmware
- [x] AV/C works through direct user-client calls (UNIT INFO answers with Yamaha's company ID)
- [x] Found which bus channel the ND-20 uses: channel 0 is allocated (matches ROUTING = 0)
- [x] Got the ND-20 manual: routing numbers, link groups, switch tables, capacity limits
- [x] Virtual Core Audio device (`src/ND20Audio/`): builds; passes silence for now (to be tested on the Mac Pro)
- [ ] Receive the ND-20 isochronous stream (decode CIP/AM824) into the virtual device
- [ ] Transmit a stream on a free routing number for playback
- [ ] Test with the full 11-unit system
- [ ] Step 1 tool: find the ND-20 and read its registers
- [ ] Receive audio from the ND-20
- [ ] Send audio to the ND-20
- [ ] Core Audio plug-in and Pro Tools testing

## Layout
- `docs/`: reverse-engineering notes
- `scripts/`: helpers to run on the Mac Pro
- `src/nd20probe/`: read-only probe tool (build with `scripts/build-probe.sh`)
- `src/ND20Audio/`: the Core Audio driver (AudioServerPlugIn); build with `scripts/build-driver.sh`
- `src/nd20claim/`: claims the device, reads the control spaces, then releases the claim (one reversible write)
- `tools/`: config ROM decoder
- `tools/re/`: analysis helpers (unpack .pkg installers on any OS)

## Trying the Core Audio driver (stage 1: silent device)
```
cd ~/Otari-mLan-Driver && git pull
bash scripts/build-driver.sh          # optional: bash scripts/build-driver.sh 64 32  (inputs outputs)
bash scripts/install-driver.sh        # asks for your Mac password
```
Open **Audio MIDI Setup**: you should see **Otari ND-20 Network** with 64 inputs and 32 outputs.
In Pro Tools, choose it under Setup > Playback Engine. Remove it with `bash scripts/uninstall-driver.sh`.
