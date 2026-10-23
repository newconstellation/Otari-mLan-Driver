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
