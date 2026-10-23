# Otari ND-20B: what the Mac sees (captured 2026-10-22, Mac Pro 5,1, Mojave)

## Config ROM (from ioreg; macOS cached only the first part)
```
Bus info: "1394", bus options 0xf0006212, GUID 0x0005420203000048
Root:  Node capabilities 0x0083c0
       Vendor ID 0x000542  "OTARI"
       Unit directory:
         Unit_Spec_ID    0x00A02D   (1394 Trade Association)
         Unit_SW_Version 0x010001   (AV/C)
         Model_ID        0x000203   "Natv ND-20B"
         Unit-dependent directory (key 0x38): not in the ioreg dump. Likely mLAN plug info.
```
Decode with: `python3 tools/parse_configrom.py <hex>`

## macOS objects that attached
- IOFireWireAVCUnit: Unit_Type 0x1C, supportsFCP = Yes
- IOFireWireAVCSubUnit: SubUnit_Type 1 = **Audio subunit**
- No audio driver attached (no AppleFWAudio), so the Mac doesn't treat it as an audio device.

## Conclusion
The ND-20 is an **AV/C + IEC 61883 device (mLAN v1 style)**, not a Yamaha "OGT" (mLAN v2) device.
- Yamaha FW Driver 1.7.5's OGT code path does **not** apply (it filters on SW version 0x000300).
- Control is through public standards: AV/C commands over FCP, plus IEC 61883-1 plug control
  registers (oPCR/iPCR) to start and stop streams.
- Audio uses IEC 61883-6 AM824 packets, the same as FFADO and Linux snd-firewire handle.

Next: `nd20probe` reads the full ROM (including the 0x38 directory), the PCRs, and the AV/C plug and format info.

## nd20probe 0.1 results (2026-10-23)
- **Full config ROM** is 64 quadlets (it repeats after that). The unit-dependent directory (key 0x38)
  holds one leaf: `0000ffff e0010000`, so the **mLAN register base is 0xFFFF E001 0000** (private space).
- **IEC 61883-1 plug registers (oMPR/iMPR) are not implemented:** both return *address error*.
  Stream setup does NOT use standard CMP; it must go through the mLAN registers above.
- **AV/C:** the IOFireWireAVCLib `open()` failed (0xe00002c0, no such device), so every command
  returned "not open". v0.2 retries with a shared device session.

Revised conclusion: the ND-20 advertises AV/C, but connection and stream control is **mLAN v1
proprietary**, via the register area at 0xFFFFE0010000. nd20probe 0.2 dumps that area (reads only).

## nd20probe 0.2 results (2026-10-23)
- Every quadlet read in 0xFFFFE0010000 .. +64 KB returns **address error**, so the area is not readable.
  Most likely it's a **write-only mailbox**: the host writes mLAN request packets there and the
  device answers by writing back to the host. That's typical of Yamaha mLAN PH-chip designs.
- AV/C: IOFireWireAVCLib refuses to open the unit (0xe00002c0) even with a shared device session.
- ROM tail (quadlets 55–63: `0001004a 40000000 …` `00060000 a8076b24 00000000 40000070 40000080`)
  is not referenced from the directory tree; meaning unknown.

Next: get the real mLAN v1 protocol from Yamaha's legacy **mLAN Driver/mLAN Tools 1.2.0 for Mac OS X**
(the generic mLAN driver for PH-chip / third-party devices) instead of probing with writes blind.

## nd20probe 0.3 results (2026-10-23): the ND-20 answers
- **Block read of 32 bytes at 0xFFFFE0000000 works** and returns a valid mLAN address map:
  `0000f000 00007d00 04000000 00000500 0d000000 0000010c 00000000 00000000`

  | Space     | Address          | Size   |
  |-----------|------------------|--------|
  | BootParam | 0xFFFF_E000_F000 | 0x7D00 |
  | Core      | 0xFFFF_E400_0000 | 0x500  |
  | mLAN      | 0xFFFF_ED00_0000 | 0x10C  |
  | App       | (none)           | 0      |
- A *quadlet* read at the same address returns `0000003f`, which doesn't match the block data. The firmware
  treats quadlet and block reads differently. Yamaha's HAL always uses **4-byte block reads**.
- 64-byte block reads of each space and the HAL's fixed registers (0xFFFFED10xxxx) returned address error.
- Generic mLAN-space layout (Enabler 1.2): +0x00 enabler address, +0x08 transporter mode,
  +0x0C+4n bandwidth request n, +0x0E+4n channel request n, +0x20 serial port data address,
  +0x28 serial port data out, +0x100 identify, +0x104 posted-write partition.
  Core space version at Core+0xC0FFC0.

nd20probe 0.4 sweeps each space with 4-byte block reads.

## nd20probe 0.4 results (2026-10-23)
- **BootParam space is readable** (flash): mostly erased `ffffffff`, one value `0060ffff` at +0xF0
  (inside the HAL-dependent area at +0xC0), and the **init table at +0x300 is all zero** (not valid).
- **Core and mLAN spaces return address error** for every 4-byte block read and quadlet read.

### Why (from mLAN Enabler 1.2, CTransporter::SetEnablerAddress)
Before touching a device, the Enabler **claims** it with a 64-bit lock (compare-and-swap) on
mLAN space + 0x00 (enabler address): compare `0xFFFF000000000000` (unowned), swap in
`(host node ID << 48) | host callback address`. It retries 5× on busy/conflict.
ClearEnablerAddress swaps it back. Most likely the ND-20 rejects other access to Core/mLAN until it's claimed.

New tool **nd20claim** (asks for confirmation): claim, then sweep mLAN/Core spaces, then release.

## nd20claim 0.2 results (2026-10-23)
- **Claim works.** The 64-bit lock on 0xFFFFED000000 succeeded; previous value 0xFFFF000000000000 (unowned).
  The release lock also succeeded. So the ND-20 implements the Enabler-address lock.
- Even while claimed, every 4-byte block read of mLAN and Core space returns address error, as do
  firmware version (0xFFFFED102000) and Core version. Yamaha's NCP08 HAL reads these with plain
  block reads (ReadBlockWithRetryBody), so the ND-20's original firmware doesn't expose them the way the
  "upgraded" firmware does.
- No notification was written to our callback space within 200 ms.

Next (nd20claim 0.3): lock-reads (CAS with compare == new) and other read sizes on mLAN space.
Next (nd20probe 0.5): find out why macOS won't let us open the AV/C connection, since the kernel itself
did talk AV/C to the ND-20 when it was detected (it found the audio subunit).

## nd20claim 0.3 / nd20probe 0.5 results (2026-10-23)
- Block reads of 8, 16 and 268 bytes on mLAN space: address error, even while claimed.
- **Lock-reads (CAS with compare == new) reported "success, old value == compare"** at every mLAN
  offset except +0x00 (which held our enabler address, so it returned kIOReturnCannotLock).
  The registers can't all really hold the test pattern, so the firmware's lock handler probably accepts any
  address and may have **stored the test pattern** (0x5A5AA5A5C3C33C3C) there. mLAN space is RAM; a
  **power cycle clears it**. Lock-reads were removed from nd20claim (v0.4). Lesson: on this firmware, a
  lock is not proof of a real register.
- AV/C: IOServiceOpen on IOFireWireAVCUnit / SubUnit **succeeds** (types 0, 1, 12). The AV/C services
  report busy state 1. nd20probe 0.6 calls Apple's AV/C user client directly (open = 0, AVC command = 4).

## nd20probe 0.6 results (2026-10-23): AV/C works via direct user-client calls
The IOFireWireAVCLib wrapper fails, but IOServiceOpen + IOConnectCall* selectors (0 = open, 4 = command) work.
| Command | Response | Meaning |
|---|---|---|
| UNIT INFO `01 ff 30 ff ff ff ff ff` | `0c ff 30 07 e0 00 a0 de` | STABLE. Unit type 0x1C, company ID **00 A0 DE (Yamaha)** |
| SUBUNIT INFO `01 ff 31 07 …` | `0c ff 31 07 ff` | no subunits listed |
| PLUG INFO (unit) | `08 …` | NOT IMPLEMENTED |
| PLUG INFO (audio subunit 0x08) | timeout | subunit doesn't answer |
| OUTPUT/INPUT PLUG SIGNAL FORMAT | `08 …` | NOT IMPLEMENTED |

Yamaha's mLAN Tools 1.2 never sends AV/C to NCP08 devices (ExecuteFCP has no callers in the Enabler
or NCP08 HAL), so the AV/C command set for this firmware isn't documented there.

Otari spec: ND-20B = IEEE 1394a **S400**, 3 ports (2 metal + 1 optical), 32–96 kHz, WCLK in.
ND-20s network with each other **without a computer**, so a standalone unit probably streams on its own.
nd20probe 0.7 reads the IRM channel/bandwidth registers and runs an AV/C GENERAL INQUIRY opcode survey.

## ND-20B rear panel (user photos, 2026-10-23)
- **ROUTING**: 4 hex rotary switches (0–F), one per slot A–D. All appear set to **0**.
- **UNIT ID**: 2 hex rotary switches (00–FF). Both appear set to **0** (Unit ID 00).
- **SETUP 1** and **SETUP 2**: two 6-way DIP switch banks (positions to be confirmed).
- **SERIAL I/O**: 8-pin mini-DIN. Possibly a remote-control or computer port (RS-232/422?). Another possible control path.
- **S400 PORT 1–3** (2 × 6-pin metal, 1 optical), **WCLK IN/OUT** BNC, EXT PWR IN, AC in.

Working hypothesis: native ND-20 networking needs no computer, so each unit probably transmits its
inputs on an isochronous channel derived from its UNIT ID, and each slot's ROUTING switch picks which
unit's stream that slot plays. If so, a Mac driver can act like "another ND-20": receive the ND-20's
broadcast and transmit its own stream for a slot whose ROUTING points at the Mac's unit ID.

## nd20probe 0.7 results (2026-10-23)
- AV/C GENERAL INQUIRY: no opcodes reported as implemented (inquiry ctype not supported by this firmware).
- IRM (Mac is IRM, node 0xffc2): **isochronous channels 0 and 31 allocated**; bandwidth 4915/4915 free.
  Channel 31 = standard broadcast channel. **Channel 0 is most likely the ND-20 (UNIT ID = 00)**,
  which fits the "channel derived from Unit ID" hypothesis. No bandwidth booked.
- Config ROM quadlet 59 changed (00060000 → 00010000) between runs, so it's a live status value.
