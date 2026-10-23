# ND-20 Operation Manual (OS1-126, 2nd ed., May 2001): notes for the driver

## How an ND-20 network moves audio
- Audio moves in **8-channel modules**. Each slot (A–D) has a **ROUTING number 0–F**.
- **Input modules** (mic amp CB-76X, AD CB-76Y, AES input) *send* on their routing number.
  **Output modules** (DA CB-77A, AES output) *play* whatever input module has the same routing number.
- One input → many outputs is allowed. **Two input modules on the same routing number is an error**:
  only one gets through, and the channel LEDs of the others light up in sequence.
- Routing works across units and within one unit (self-routing). The system starts streaming automatically
  at power-up; no computer or controller is needed.
- **Inference (to verify):** routing number N most likely maps to IEEE 1394 isochronous channel N.
  Supporting evidence: the test unit has all ROUTING = 0, and channel 0 is the only channel it allocates.

## Capacity limits
- Per unit: 32 ch at ≤48 kHz (4 slots), 16 ch at 88.2/96 kHz (slots B and D only; mic amp in A/C).
- **Whole network: max 96 channels at 48 kHz** (48 at 96 kHz). Links are IEEE 1394 **S200**.
- Max 63 devices, < 16 hops; daisy chain up to 17 units.
- Implication for the Mac: **listening costs no bus bandwidth**, so it can record every routing number in use.
  What it *sends* counts against the 96-channel budget, and each 8 channels it sends needs a free routing number.
  With all 64 mics active, ~32 channels (4 routing numbers) are left for playback from the Mac.

## Rear panel
| Control | Meaning |
|---|---|
| UNIT ID (00–FF) | Unique per unit; read at power-up only. Duplicates → ALARM + red LEDs. |
| ROUTING A–D | Routing number per slot (changeable live). Mic amp module counts as slot A or C. |
| SETUP 1-1..1-4 | Link group 0–15 (binary, 1-1 = MSB). All OFF = group 0. |
| SETUP 1-5 | ON = link-group **master**, OFF = slave. A standalone unit doesn't care. |
| SETUP 1-6 | External word-clock sync ON/OFF. |
| SETUP 2-1..2-3 | Sample rate: OFF/OFF/OFF = **48k**, ON/OFF/OFF = 44.1k, OFF/ON/OFF = 32k, OFF/OFF/ON = 96k, ON/OFF/ON = 88.2k. |
| SETUP 2-4 | Restore mic-amp settings at power-up when no CB-178 is present. |
| SETUP 2-5 | Unused (OFF). |
| SETUP 2-6 | Accept firmware update (OFF normally). |
| SERIAL I/O | **Firmware update only**, not a control port. |
| WORD IN / OUT | WCLK in (ext sync) and out (current clock; not a thru). |

Test unit (all SETUP OFF): link group 0, slave, 48 kHz, ext sync off.
Note: its two CB-76Y cards are **both on ROUTING 0**, which the manual calls a routing error.

## Clocking
- Each **link group** (up to 16) has one master; slaves lock to the master's clock carried over 1394.
- Routing between groups at different rates/clocks → noise. **The Mac must slave to the group master's
  stream timing** (the SYT timestamps in the received stream), not run its own clock.

## CB-178 remote
- 1394 device (3 metal ports), bus-powered (12 W max). Only **one CB-178 per network**.
- Controls mic amp per channel: gain +13…+62 dB (pad off) in 0.5 dB steps, −25 dB pad, +48V, limiter.
  Plus groups, assignment pages, naming, setup save/recall, and monitoring any routed channel on its
  meter/headphones (so it can *receive* any routing number's audio, like the Mac would).
- Mic settings are stored in each ND-20. Its control messages travel over 1394 (protocol undocumented).

## Errors (ALARM LED)
AES input slip, DA power-down (clock problem), 1394 bus/port error (cable change, master clock change),
1394 audio data error (routing change / lost destination).
