# Yamaha Steinberg FW Driver 1.7.5 — RE notes (for ND-20 driver project)

## Package layout
- YamahaFWAudioDriver.kext (x86_64) — IOAudio stream engine. Matches IOFireWireLocalNode (the Mac's own FW controller), NOT device IDs. UserClient + CFPlugIn YamahaFWUserLib.
- YamahaFWAsync.kext (x86_64) — async FW transactions / address space for device→host messages.
- YamahaFWEnabler.framework (i386 only) — generic mLAN "Enabler" (connection manager core). Ships C++ headers.
- YamahaFWHAL.bundle (ppc+i386) — CHAL_OGT_PJ, CFPlugIn factory 4AC0557E-..., interface IID 2FC71D29-...
- YamahaFWCM.app (i386) — connection manager daemon (LaunchAgent).
- YamahaFWMIDI.plugin — CoreMIDI driver. Control panel = Qt4 app.

## Device acceptance (the "whitelist")
GetMLANConfigROM() @ Enabler 0xc49c: device is accepted only if a config-ROM unit directory has
- Unit_Spec_ID == 0x00A02D
- Unit_SW_Version == 0x000300
- HAL model ID == 0x10000F   (struct SConfigROMUnitInfo offset 0x550)
Nothing else in the stack filters by model; kext is device-agnostic.

## Protocol model (HAL)
CGenericTransporter over "OGT" (Open Generic Transporter) register spaces:
CoreSpace, ConfigurationSpace, ISPSpace (iso stream plugs), NCPSpace, NodeApplicationSpace,
NodeControllerSpace, SyncSpace (SyncSrc/SyncGrpSrc/SyncOutMod), CommSpace, PrivateSpace, PlugLayoutTable.
Base comes from config-ROM Unit_Location (unitLocationBase/Bound).
Enabler↔HAL selector table: EnablerHalIFDef.h (kMLANTrSelector_*). Stream structs: PJDef.h.
Phase alignment: AdjustInputPhase{SS,SM,MS} using SYT offsets (IEC 61883-6).

## Open question
ND-20 config ROM: spec/version/HAL IDs, Unit_Location. Decides whether OGT path applies
(patch the filter + reuse model) or ND-20 is mLAN v1 / PH-chip protocol (different control path).
