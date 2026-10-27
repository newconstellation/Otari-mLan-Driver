# Stage 2: receiving the ND-20's audio (nd20rx, nd20listen, ND20Audio)

## Design (2026-10-23)
- **Receive only.** The Mac listens; it writes nothing to the ND-20 and books no bandwidth. The isochronous
  channel object is created with `doIRM = false`, because the ND-20 already allocated its channel at the IRM.
- **Which FireWire object is opened:** the Mac's own node (`IOFireWireLocalNode`) through IOFireWireLib.
  If that can't be opened, the ND-20 device itself is opened instead.
- **DMA program:** per channel, a ring of 512 NuDCL "receive packet" commands (64 ms at 8000 packets/s),
  each storing the 4-byte isochronous header plus up to 2052 bytes of payload, and a time stamp.
  Every 16th DCL has a callback (every 2 ms) with `kNuDCLUpdateBeforeCallback`, so the header and time stamp
  are valid when the packets are decoded.
- A **remote isoch port** stands in for the ND-20 as the talker; its GetSupported handler returns only the
  wanted channel (bit 63 = channel 0, as in IOFWIsochChannel.cpp).
- **Decoding:** isochronous header (data_length, channel, tcode 0xA; byte order detected per packet),
  CIP (SID, DBS, DBC, FMT, FDF, SYT), AM824 quadlets. Audio slots = labels 0x40–0x4F (MBLA);
  if none, labels 0x00–0x3F (IEC 60958). Up to 16 audio slots kept per stream; the driver uses the first 8.
  Sample rate from FDF bits 0–2. Lost packets counted from DBC jumps.
- **Clock:** the first stream with audio becomes the clock master. A least-squares fit of
  (frames received, host time) over the last ~1 s gives the ND-20's sample rate in Mac host time.
  The driver's zero time stamps come from this fit, so Core Audio runs on the ND-20's clock.
- **Aligning several streams:** each stream's offset to the master is first estimated from arrival times,
  then made exact by matching SYT presentation time stamps (equal SYT = same instant, mod 16 bus cycles).
- **Driver:** routing N → inputs 8N+1..8N+8. The channels to listen on are chosen at build time
  (`bash scripts/build-driver.sh 64 32 0,1`). If nothing can be received within 1.5 s of starting,
  the device falls back to the Mac's clock and silent inputs, and logs why.
- coreaudiod is sandboxed: `Info.plist` lists `IOFireWireUserClient` under `AudioServerPlugIn_IOKitClients`
  so the plug-in may open FireWire.

## How to read nd20listen's output
| You see | Meaning |
|---|---|
| ~8000 pkts/s on a channel | The ND-20 transmits on that channel (one packet per bus cycle). |
| 0 pkts/s, "No packets at all" | Nothing is sent on that channel, or reception failed (check the setup messages). |
| "isoch header byte order: MISSING" | Packets arrive but the header isn't where expected; send the "First packets" dump. |
| FMT not 0x10 | The ND-20 doesn't use AM824 on this channel; send the dump. |
| audio channels: 8, labels 0x40 | Standard 24-bit MBLA audio: what we expect for a CB-76Y card. |
| lost > 0 | Packets were dropped (DBC jumped). A few at start-up are normal; a steady count is not. |
| meters move with your signal | Audio decodes correctly. Check the WAV files with `-w`. |

## Results
### 2026-10-25 sweep (nd20listen -s, groups of 16)
- In every group only the first **7 channels** opened; the rest failed with `0xe00002be` (no resources).
  The Mac Pro's FireWire controller has a limited number of isochronous **receive contexts** (7 free; Apple
  probably keeps one for broadcast channel 31). One context per channel means **at most 7 routing numbers
  = 56 inputs**. 64 inputs (8 routings) or the full 96-channel network will need **multi-channel receive**
  (one OHCI context receiving several channels). To solve before the full system goes on.
- **No packets on any channel that opened, including 0 and 1.** Either the ND-20 isn't transmitting
  (e.g. nothing on the network plays its routing numbers) or nd20rx isn't receiving. nd20listen 0.2 adds:
  sweep in groups of 6, a "DMA slots used" check (data reached memory even if callbacks didn't run),
  and `-a`, a second independent receive path through Apple's async stream listener.

### 2026-10-25 with tone on inputs 1–8 (nd20listen 0.2)
- nd20rx on channels 0 and 1: **0 packets**. Apple's own async stream listener on 0 and 1: **0 packets**.
  Sweep of all 64 channels in groups of 6 (all opened): **0 packets, no data in any DMA buffer**.
- Two independent receive paths agree: **the ND-20 is not putting any isochronous packets on the bus.**
  The receive code is not the suspect; the question is what makes an ND-20 start transmitting.
- Hypotheses, cheapest test first:
  1. **No clock master:** the unit is a link-group slave (SETUP 1-5 OFF) and nothing on the bus is master,
     so it may wait for a master's clock and never transmit. Test: SETUP 1-5 ON, power-cycle, listen again.
  2. **Transmits on demand:** an ND-20 may only stream a routing number when something wants it
     (an output module on that routing, or the CB-181's ROUTING MONITOR). Test: ROUTING MONITOR on routing 0
     during nd20listen; then a line-out ND-20 with a slot on routing 0.
  3. If a second ND-20 makes it stream, the "request" between units is the next protocol to capture.
- Follow-up tests, same day: ND-20 **power-cycled** with the chain connected: 0 packets.
  (A second run labelled "master" was taken with SETUP 1-5 still OFF: 0 packets; the master test is **not done yet**.)
  **ROUTING MONITOR** on routing 0 from the CB-181 during listening: 0 packets for the first 11 s captured.
  The bus-reset idea is ruled out. **SETUP 1-5 ON** (link-group master) + power-cycle, tone on 1–8: **0 packets** (8 s captured), so hypothesis 1 is ruled out too. Next (2026-10-26): add a CB-77A line-out ND-20 with a slot on routing 0.

### 2026-10-27: three units (line-in UNIT 00 master, mic-amp UNIT 01, line-out UNIT 02) + CB-181
- **First isochronous stream seen.** Channel 0, ~8100 packets/s, 0 lost, from **SID 2** = node 0xffc2 = the
  line-input ND-20 (GUID ...0048). Standard IEC 61883-6: CIP `0201xxxx 9002yyyy` (SID 2, **DBS 1**, FMT 0x10
  AM824, FDF 0x02 = 48 kHz, SYT present on 3 of 4 packets), 5–7 data blocks per packet, label 0x40 (MBLA 24-bit).
  nd20rx decoded it cleanly (isoch header stored little-endian by Apple's DMA; handled).
- **Only ONE audio channel** in the stream (DBS 1), and it is the converter's noise floor: peak −101 dBFS,
  RMS −115 dBFS, no tone in the WAV. Not the 8 channels of routing 0 with tone.
- Channels 1, 2, 3: nothing. Sweep of all 64: only channel 0 (plus a single stray packet on channel 4).
- Hypothesis: this mono stream is the **CB-181's ROUTING MONITOR feed** (one channel, sent to the remote),
  not inter-unit audio. To confirm: toggle ROUTING MONITOR and change the monitored channel while listening.
- Clock fit reported 48.4–49.0 kHz at lock (should be 48.0): lock is declared too early; lengthen the fit.
- Scan: new units' GUIDs 0x0005420203000005 (node 0xffc4) and 0x0005420203000003 (node 0xffc5), both
  model 0x203 "Natv ND-20B"; ROM quadlet 59 = 0x00060000 on both, 0x00050000 on the line-input unit.
- nd20remote watch with the remote in use: 0 events (the remote still sends nothing to the Mac).
