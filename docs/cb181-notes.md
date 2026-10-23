# Otari CB-181 Remote Control Unit: notes for a software version

The user's remote is a **CB-181** (the ND-20 manual names the remote CB-178; the CB-181 is presumably a
later or alternate model of the same remote). FireWire device, one per network.

## Front panel (from photos, 2026-10-23)
- Backlit LCD with soft labels **1–8** above and below: the remote works on **8 channels at a time** (one module).
- **CLIP** LEDs 1–8 (one per channel on the current page).
- **No.** 7-segment digit + 24-segment bar meter (−60 … 0 dB): meters **one** selected channel.
- Mode keys: **SETUP, NAME, GROUP, ASSIGN, ROUTING MONITOR**.
- Channel keys: **PAD** (−25 dB), **+48V**, **LIMITER**, **GAIN** (+13 … +62 dB in 0.5 dB steps).
- Navigation: **← / →**, **CANCEL**, **ENTER**, large rotary encoder (jog wheel).

## Goal: software remote
- Same functions as the CB-181 (gain, pad, +48V, limiter, names, groups, setup save/recall, routing monitor).
- **All-channel meter bridge**: every routing number in use, metered at once (up to 96 channels at 48 kHz).
  This needs no remote protocol: the Mac already receives the audio and nd20rx keeps a peak per channel,
  and listening costs no bus bandwidth. Clip indicators from the same data.

## Why the protocol has to be discovered
The manual says control messages travel over 1394 but doesn't describe them. SERIAL I/O is firmware-update only.
The Mac's OHCI controller only receives packets addressed to the Mac (or broadcast), so CB-181 → ND-20
transactions can't simply be sniffed.

## Plan
1. `nd20remote scan` (read only): CB-181's IDs, unit directories and config ROM.
2. `nd20listen -s` (receive only): sweep all 64 isochronous channels while using the remote
   (meters / routing-monitor audio may travel as streams).
3. `nd20remote watch` (safe with everything connected): logs any write sent to the Mac in the ND-20
   register ranges, including broadcasts.
4. `nd20remote pose` (**ND-20s unplugged**): the Mac publishes a unit directory with the ND-20's IDs
   (spec 0x00A02D, version 0x010001, model 0x000203), answers the ND-20 register ranges with the values
   read from the real unit, and logs every read, write and lock from the CB-181.
   Limitation: the Mac's root directory still says vendor Apple. If the CB-181 checks the root vendor,
   it will ignore the Mac; the fallback is a bus analyser (Linux + TI PCILynx card + `nosy`).
5. Replay one decoded command to a real ND-20 (e.g. gain on an unused mic channel) to confirm.
6. Build the software remote + meter bridge.

## Implementation note (pose mode)
IOFireWireLib passes LOCK requests on fixed-address pseudo spaces to the read handler with the
*absolute* low address as the offset, then compares against backing store + that address. nd20remote puts
each backing store at the start of a 4 GB virtual reservation and mirrors register values at
store + absolute address, so locks (e.g. the Enabler claim at mLAN+0x00) behave like the real ND-20.
The kernel ignores the completion status for reads and locks and always answers from the backing store.

## Results
(fill in after running on the Mac Pro)
