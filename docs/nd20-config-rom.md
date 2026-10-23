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
