# Yamaha mLAN Tools 1.2.0 for Mac OS X: RE notes

Package: Enabler framework (ppc+i386), HAL plug-ins per device, kernel driver, Graphic Patchbay.
Relevant HAL: **HAL-00a0de-000002 "Generic NCP08 HAL"**, described as "Drivers for upgraded
first generation mLAN products". NCP08 hardware = **NC1** node controller (Hitachi SH-2 CPU)
+ **PH1** audio/packet chip + **MD8413** FireWire link layer.

## Device acceptance (Enabler 1.2, GetMLANConfigROM)
A unit directory must be either
- Unit_Spec_ID 0x00A02D and Unit_SW_Version 0x000300 (mLAN v2), or
- Unit_Spec_ID 0x00A0DE (Yamaha) and Unit_SW_Version 0xFFFFFF.

The ND-20B reports 0x00A02D / **0x010001** (AV/C), so it is **not** accepted. Its firmware looks
like the original mLAN v1 firmware, not the "upgraded" one. Otari's site no longer offers ND-20 files.

## Address map
CTransporter constructor: **block read of 32 bytes at 0xFFFF_E000_0000**, retried up to 5 times on
"conflict"/busy. Eight big-endian quadlets = (offset, size) pairs relative to 0xFFFF_E000_0000:
BootParam space, Core space, mLAN space, App space.

| Register (NCP08 HAL)            | Address                          |
|---------------------------------|----------------------------------|
| Core ROM                        | Core + 0x000000                  |
| PH1 registers                   | Core + 0x800000                  |
| MD8413 link registers           | Core + 0xC00000                  |
| DRAM                            | Core + 0x1000000                 |
| SH-2 internal devices / RAM     | Core + 0xFFFF8000 / 0xFFFFF000   |
| mLAN transporter mode           | mLAN + 0x08                      |
| mLAN bandwidth request          | mLAN + 0x0C                      |
| mLAN channel request            | mLAN + 0x10                      |
| Posted-write partition offset   | mLAN + 0x104                     |
| Boot params HAL-dependent area  | BootParam + 0xC0                 |
| Init table / validity flag      | BootParam + 0x300                |
| CSR RESET_START                 | 0xFFFF_F000_000C                 |
| Firmware version                | 0xFFFF_ED10_2000                 |
| PH1 Rx delay offset             | 0xFFFF_ED10_1000                 |
| PH1 Tx control                  | 0xFFFF_ED10_1010                 |
| Error register                  | 0xFFFF_ED10_1014                 |

HAL properties (show what's configurable): PH1DataFormat, PH1BitDepth, PH1ExtClockFormat,
PH1ExtClockEnable, PH1AutoMute, audioInput/OutputChanMask, WordClockMaster, ActualSFC.

nd20probe 0.3 tests whether the ND-20's firmware answers these reads.
