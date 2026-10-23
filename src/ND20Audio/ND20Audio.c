/*
 * ND20Audio: Core Audio (AudioServerPlugIn) driver for an Otari ND-20 network.
 *
 * Publishes one audio device, "Otari ND-20 Network", with ND20_INPUTS recording channels
 * and ND20_OUTPUTS playback channels, at 44.1 or 48 kHz.
 *
 * Stage 2 (receive): when IO starts, the nd20rx engine listens on the isochronous channels in
 *   ND20_RX_CHANNELS (ND-20 routing numbers). Routing N fills inputs 8N+1 .. 8N+8. The device
 *   clock follows the ND-20's stream (the first stream that locks is the clock master).
 *   If no ND-20 stream can be received, the device falls back to the Mac's clock and silence.
 *   Outputs are still discarded (transmit is the next stage).
 *
 * Channel names follow ND-20 routing: 8 channels per routing number, e.g. "In 9 (Rt 1 Ch 1)".
 * Log messages:  log stream --predicate 'subsystem == "com.newconstellation.ND20Audio"'
 *
 * Object layout:   1 = plug-in   2 = device   3 = input stream   4 = output stream
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include "../nd20rx/nd20rx.h"

#ifndef ND20_INPUTS
#define ND20_INPUTS 64
#endif
#ifndef ND20_OUTPUTS
#define ND20_OUTPUTS 32
#endif
#ifndef ND20_RX_CHANNELS
#define ND20_RX_CHANNELS 0,1           /* isochronous channels (= ND-20 routing numbers) to receive */
#endif
#define kInputSafetyOffset    384u     /* frames: received audio arrives in 2 ms bursts */
#define kLockWaitMs           1500     /* how long StartIO waits for the ND-20 clock */

#define kPlugInBundleID       "com.newconstellation.ND20Audio"
#define kDeviceUID            "ND20Audio_Device_UID"
#define kDeviceModelUID       "ND20Audio_Model_UID"
#define kDeviceName           "Otari ND-20 Network"
#define kManufacturer         "Otari ND-20 (unofficial driver)"
#define kZeroTimeStampPeriod  16384u
#define kBytesPerSample       4u

enum {
    kObjectID_PlugIn    = kAudioObjectPlugInObject,   /* 1 */
    kObjectID_Device    = 2,
    kObjectID_StreamIn  = 3,
    kObjectID_StreamOut = 4,
};

static const Float64 kSampleRates[] = {44100.0, 48000.0};
#define kNumSampleRates (sizeof kSampleRates / sizeof kSampleRates[0])

/* ------------------------------------------------------------------ state */

static pthread_mutex_t          gStateMutex = PTHREAD_MUTEX_INITIALIZER;
static AudioServerPlugInHostRef gHost = NULL;
static UInt32                   gRefCount = 0;

static Float64  gSampleRate = 48000.0;
static UInt32   gIOCount = 0;               /* number of clients running IO */
static Boolean  gStreamInActive = true;
static Boolean  gStreamOutActive = true;

/* Clock: the Mac's host clock (fallback) or the ND-20 stream (when gUseRx). */
static Float64  gHostTicksPerFrame = 0.0;
static UInt64   gAnchorHostTime = 0;
static UInt64   gNumberTimeStamps = 0;

/* FireWire receive (stage 2) */
static const int  kRxChannels[] = { ND20_RX_CHANNELS };
#define kNumRxChannels ((int)(sizeof kRxChannels / sizeof kRxChannels[0]))
static ND20Rx      *gRx = NULL;
static _Atomic int  gUseRx = 0;        /* 1 while the device clock follows the ND-20 */
static Float64      gFrameBase = 0.0;  /* master stream frame that is device sample time 0 */
static UInt64       gZeroHost = 0;     /* host time of the current zero time stamp */
static os_log_t     gLogObj = NULL;

static void RxLog(const char *msg) {
    if (gLogObj) os_log(gLogObj, "%{public}s", msg);
}

/* --------------------------------------------------------- forward decls */

static HRESULT  ND20_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface);
static ULONG    ND20_AddRef(void *inDriver);
static ULONG    ND20_Release(void *inDriver);
static OSStatus ND20_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus ND20_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *);
static OSStatus ND20_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus ND20_AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus ND20_RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus ND20_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static OSStatus ND20_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static Boolean  ND20_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
static OSStatus ND20_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
static OSStatus ND20_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *);
static OSStatus ND20_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, UInt32 *, void *);
static OSStatus ND20_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, const void *);
static OSStatus ND20_StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus ND20_StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus ND20_GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
static OSStatus ND20_WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
static OSStatus ND20_BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);
static OSStatus ND20_DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *, void *, void *);
static OSStatus ND20_EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);

static AudioServerPlugInDriverInterface gDriverInterface = {
    NULL,
    ND20_QueryInterface, ND20_AddRef, ND20_Release,
    ND20_Initialize, ND20_CreateDevice, ND20_DestroyDevice,
    ND20_AddDeviceClient, ND20_RemoveDeviceClient,
    ND20_PerformDeviceConfigurationChange, ND20_AbortDeviceConfigurationChange,
    ND20_HasProperty, ND20_IsPropertySettable, ND20_GetPropertyDataSize,
    ND20_GetPropertyData, ND20_SetPropertyData,
    ND20_StartIO, ND20_StopIO, ND20_GetZeroTimeStamp,
    ND20_WillDoIOOperation, ND20_BeginIOOperation, ND20_DoIOOperation, ND20_EndIOOperation
};
static AudioServerPlugInDriverInterface *gDriverInterfacePtr = &gDriverInterface;
static AudioServerPlugInDriverRef        gDriverRef = &gDriverInterfacePtr;

/* --------------------------------------------------------------- factory */

void *ND20Audio_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID);
void *ND20Audio_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID) {
    (void)inAllocator;
    if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) return gDriverRef;
    return NULL;
}

/* ------------------------------------------------------------- IUnknown */

static HRESULT ND20_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface) {
    if (inDriver != gDriverRef || outInterface == NULL) return kAudioHardwareBadObjectError;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    HRESULT result = E_NOINTERFACE;
    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
        pthread_mutex_lock(&gStateMutex);
        ++gRefCount;
        pthread_mutex_unlock(&gStateMutex);
        *outInterface = gDriverRef;
        result = S_OK;
    }
    CFRelease(requested);
    return result;
}

static ULONG ND20_AddRef(void *inDriver) {
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    ULONG r = ++gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return r;
}

static ULONG ND20_Release(void *inDriver) {
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    if (gRefCount > 0) --gRefCount;
    ULONG r = gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return r;
}

/* ------------------------------------------------------- basic operations */

static void UpdateClockForRate(void) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    Float64 ticksPerSecond = 1.0e9 * (Float64)tb.denom / (Float64)tb.numer;
    gHostTicksPerFrame = ticksPerSecond / gSampleRate;
}

static OSStatus ND20_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    gHost = inHost;
    UpdateClockForRate();
    gLogObj = os_log_create(kPlugInBundleID, "driver");
    nd20rx_set_log(RxLog);
    return kAudioHardwareNoError;
}

static OSStatus ND20_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *ci, AudioObjectID *out) {
    (void)d; (void)desc; (void)ci; (void)out;
    return kAudioHardwareUnsupportedOperationError;
}
static OSStatus ND20_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id) {
    (void)d; (void)id;
    return kAudioHardwareUnsupportedOperationError;
}
static OSStatus ND20_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo *ci) {
    (void)ci;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (id != kObjectID_Device) return kAudioHardwareBadObjectError;
    return kAudioHardwareNoError;
}
static OSStatus ND20_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo *ci) {
    (void)ci;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (id != kObjectID_Device) return kAudioHardwareBadObjectError;
    return kAudioHardwareNoError;
}

/* Configuration changes: the action code is the new sample rate in Hz. */
static OSStatus ND20_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 action, void *info) {
    (void)info;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (id != kObjectID_Device) return kAudioHardwareBadDeviceError;
    Float64 rate = (Float64)action;
    Boolean ok = false;
    for (UInt32 i = 0; i < kNumSampleRates; i++) if (kSampleRates[i] == rate) ok = true;
    if (!ok) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gStateMutex);
    gSampleRate = rate;
    UpdateClockForRate();
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}
static OSStatus ND20_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 action, void *info) {
    (void)action; (void)info;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (id != kObjectID_Device) return kAudioHardwareBadDeviceError;
    return kAudioHardwareNoError;
}

/* ------------------------------------------------------ helper functions */

static UInt32 ChannelsForStream(AudioObjectID stream) {
    return stream == kObjectID_StreamIn ? ND20_INPUTS : ND20_OUTPUTS;
}

static void FillASBD(AudioStreamBasicDescription *f, UInt32 channels, Float64 rate) {
    memset(f, 0, sizeof *f);
    f->mSampleRate       = rate;
    f->mFormatID         = kAudioFormatLinearPCM;
    f->mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f->mBytesPerPacket   = kBytesPerSample * channels;
    f->mFramesPerPacket  = 1;
    f->mBytesPerFrame    = kBytesPerSample * channels;
    f->mChannelsPerFrame = channels;
    f->mBitsPerChannel   = 32;
}

/* "In 9 (Rt 1 Ch 1)": 8 channels per ND-20 routing number. */
static CFStringRef CopyChannelName(Boolean isInput, UInt32 element) {
    UInt32 idx = element - 1;
    return CFStringCreateWithFormat(NULL, NULL, CFSTR("%s %u (Rt %X Ch %u)"),
                                    isInput ? "In" : "Out", (unsigned)element, (unsigned)(idx / 8), (unsigned)(idx % 8 + 1));
}

#define CHECK_SIZE(needed) do { if (inDataSize < (needed)) return kAudioHardwareBadPropertySizeError; } while (0)

/* --------------------------------------------------------- HasProperty */

static Boolean ND20_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a) {
    (void)pid;
    if (d != gDriverRef || a == NULL) return false;
    switch (obj) {
    case kObjectID_PlugIn:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
            return true;
        }
        return false;
    case kObjectID_Device:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID: case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices: case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive: case kAudioDevicePropertyDeviceIsRunning:
        case kAudioObjectPropertyControlList: case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates: case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod: case kAudioDevicePropertyStreams:
            return true;
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency: case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyPreferredChannelsForStereo:
            return a->mScope == kAudioObjectPropertyScopeInput || a->mScope == kAudioObjectPropertyScopeOutput;
        case kAudioObjectPropertyElementName:
            if (a->mScope == kAudioObjectPropertyScopeInput)  return a->mElement >= 1 && a->mElement <= ND20_INPUTS;
            if (a->mScope == kAudioObjectPropertyScopeOutput) return a->mElement >= 1 && a->mElement <= ND20_OUTPUTS;
            return false;
        }
        return false;
    case kObjectID_StreamIn:
    case kObjectID_StreamOut:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects: case kAudioObjectPropertyName:
        case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
        }
        return false;
    }
    return false;
}

/* --------------------------------------------------- IsPropertySettable */

static OSStatus ND20_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out) {
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (a == NULL || out == NULL) return kAudioHardwareIllegalOperationError;
    if (!ND20_HasProperty(d, obj, pid, a)) return kAudioHardwareUnknownPropertyError;
    *out = false;
    if (obj == kObjectID_Device && a->mSelector == kAudioDevicePropertyNominalSampleRate) *out = true;
    if ((obj == kObjectID_StreamIn || obj == kObjectID_StreamOut) &&
        (a->mSelector == kAudioStreamPropertyIsActive || a->mSelector == kAudioStreamPropertyVirtualFormat ||
         a->mSelector == kAudioStreamPropertyPhysicalFormat)) *out = true;
    return kAudioHardwareNoError;
}

/* --------------------------------------------------- GetPropertyDataSize */

static OSStatus ND20_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                         UInt32 qSize, const void *qData, UInt32 *outSize) {
    (void)qSize; (void)qData;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (a == NULL || outSize == NULL) return kAudioHardwareIllegalOperationError;
    if (!ND20_HasProperty(d, obj, pid, a)) return kAudioHardwareUnknownPropertyError;

    switch (obj) {
    case kObjectID_PlugIn:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outSize = sizeof(AudioClassID); break;
        case kAudioObjectPropertyOwner: case kAudioPlugInPropertyTranslateUIDToDevice: *outSize = sizeof(AudioObjectID); break;
        case kAudioObjectPropertyManufacturer: case kAudioPlugInPropertyResourceBundle: *outSize = sizeof(CFStringRef); break;
        case kAudioObjectPropertyOwnedObjects: case kAudioPlugInPropertyDeviceList: *outSize = sizeof(AudioObjectID); break;
        }
        return kAudioHardwareNoError;

    case kObjectID_Device:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outSize = sizeof(AudioClassID); break;
        case kAudioObjectPropertyOwner: *outSize = sizeof(AudioObjectID); break;
        case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID:
        case kAudioObjectPropertyElementName: *outSize = sizeof(CFStringRef); break;
        case kAudioObjectPropertyOwnedObjects: case kAudioDevicePropertyStreams:
            if (a->mScope == kAudioObjectPropertyScopeGlobal) *outSize = 2 * sizeof(AudioObjectID);
            else *outSize = sizeof(AudioObjectID);
            break;
        case kAudioDevicePropertyTransportType: case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive: case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency: case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod: *outSize = sizeof(UInt32); break;
        case kAudioDevicePropertyRelatedDevices: *outSize = sizeof(AudioObjectID); break;
        case kAudioObjectPropertyControlList: *outSize = 0; break;
        case kAudioDevicePropertyNominalSampleRate: *outSize = sizeof(Float64); break;
        case kAudioDevicePropertyAvailableNominalSampleRates: *outSize = kNumSampleRates * sizeof(AudioValueRange); break;
        case kAudioDevicePropertyPreferredChannelsForStereo: *outSize = 2 * sizeof(UInt32); break;
        }
        return kAudioHardwareNoError;

    case kObjectID_StreamIn:
    case kObjectID_StreamOut:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outSize = sizeof(AudioClassID); break;
        case kAudioObjectPropertyOwner: *outSize = sizeof(AudioObjectID); break;
        case kAudioObjectPropertyOwnedObjects: *outSize = 0; break;
        case kAudioObjectPropertyName: *outSize = sizeof(CFStringRef); break;
        case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency: *outSize = sizeof(UInt32); break;
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
            *outSize = sizeof(AudioStreamBasicDescription); break;
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
            *outSize = kNumSampleRates * sizeof(AudioStreamRangedDescription); break;
        }
        return kAudioHardwareNoError;
    }
    return kAudioHardwareBadObjectError;
}

/* ------------------------------------------------------- GetPropertyData */

static OSStatus ND20_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                     UInt32 qSize, const void *qData, UInt32 inDataSize, UInt32 *outSize, void *outData) {
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (a == NULL || outSize == NULL || outData == NULL) return kAudioHardwareIllegalOperationError;
    if (!ND20_HasProperty(d, obj, pid, a)) return kAudioHardwareUnknownPropertyError;

    switch (obj) {
    /* ---------------- plug-in */
    case kObjectID_PlugIn:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
            CHECK_SIZE(sizeof(AudioClassID)); *(AudioClassID *)outData = kAudioObjectClassID; *outSize = sizeof(AudioClassID); break;
        case kAudioObjectPropertyClass:
            CHECK_SIZE(sizeof(AudioClassID)); *(AudioClassID *)outData = kAudioPlugInClassID; *outSize = sizeof(AudioClassID); break;
        case kAudioObjectPropertyOwner:
            CHECK_SIZE(sizeof(AudioObjectID)); *(AudioObjectID *)outData = kAudioObjectUnknown; *outSize = sizeof(AudioObjectID); break;
        case kAudioObjectPropertyManufacturer:
            CHECK_SIZE(sizeof(CFStringRef)); *(CFStringRef *)outData = CFSTR(kManufacturer); *outSize = sizeof(CFStringRef); break;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
            if (inDataSize < sizeof(AudioObjectID)) { *outSize = 0; break; }
            *(AudioObjectID *)outData = kObjectID_Device; *outSize = sizeof(AudioObjectID); break;
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            CHECK_SIZE(sizeof(AudioObjectID));
            AudioObjectID result = kAudioObjectUnknown;
            if (qSize == sizeof(CFStringRef) && qData != NULL) {
                CFStringRef uid = *(const CFStringRef *)qData;
                if (uid && CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo) result = kObjectID_Device;
            }
            *(AudioObjectID *)outData = result; *outSize = sizeof(AudioObjectID); break;
        }
        case kAudioPlugInPropertyResourceBundle:
            CHECK_SIZE(sizeof(CFStringRef)); *(CFStringRef *)outData = CFSTR(""); *outSize = sizeof(CFStringRef); break;
        default: return kAudioHardwareUnknownPropertyError;
        }
        return kAudioHardwareNoError;

    /* ---------------- device */
    case kObjectID_Device:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
            CHECK_SIZE(sizeof(AudioClassID)); *(AudioClassID *)outData = kAudioObjectClassID; *outSize = sizeof(AudioClassID); break;
        case kAudioObjectPropertyClass:
            CHECK_SIZE(sizeof(AudioClassID)); *(AudioClassID *)outData = kAudioDeviceClassID; *outSize = sizeof(AudioClassID); break;
        case kAudioObjectPropertyOwner:
            CHECK_SIZE(sizeof(AudioObjectID)); *(AudioObjectID *)outData = kObjectID_PlugIn; *outSize = sizeof(AudioObjectID); break;
        case kAudioObjectPropertyName:
            CHECK_SIZE(sizeof(CFStringRef)); *(CFStringRef *)outData = CFSTR(kDeviceName); *outSize = sizeof(CFStringRef); break;
        case kAudioObjectPropertyManufacturer:
            CHECK_SIZE(sizeof(CFStringRef)); *(CFStringRef *)outData = CFSTR(kManufacturer); *outSize = sizeof(CFStringRef); break;
        case kAudioDevicePropertyDeviceUID:
            CHECK_SIZE(sizeof(CFStringRef)); *(CFStringRef *)outData = CFSTR(kDeviceUID); *outSize = sizeof(CFStringRef); break;
        case kAudioDevicePropertyModelUID:
            CHECK_SIZE(sizeof(CFStringRef)); *(CFStringRef *)outData = CFSTR(kDeviceModelUID); *outSize = sizeof(CFStringRef); break;
        case kAudioObjectPropertyElementName:
            CHECK_SIZE(sizeof(CFStringRef));
            *(CFStringRef *)outData = CopyChannelName(a->mScope == kAudioObjectPropertyScopeInput, a->mElement);
            *outSize = sizeof(CFStringRef); break;
        case kAudioDevicePropertyTransportType:
            CHECK_SIZE(sizeof(UInt32)); *(UInt32 *)outData = kAudioDeviceTransportTypeFireWire; *outSize = sizeof(UInt32); break;
        case kAudioDevicePropertyRelatedDevices:
            if (inDataSize < sizeof(AudioObjectID)) { *outSize = 0; break; }
            *(AudioObjectID *)outData = kObjectID_Device; *outSize = sizeof(AudioObjectID); break;
        case kAudioDevicePropertyClockDomain:
            CHECK_SIZE(sizeof(UInt32)); *(UInt32 *)outData = 0; *outSize = sizeof(UInt32); break;
        case kAudioDevicePropertyDeviceIsAlive:
            CHECK_SIZE(sizeof(UInt32)); *(UInt32 *)outData = 1; *outSize = sizeof(UInt32); break;
        case kAudioDevicePropertyDeviceIsRunning:
            CHECK_SIZE(sizeof(UInt32));
            pthread_mutex_lock(&gStateMutex); *(UInt32 *)outData = gIOCount > 0; pthread_mutex_unlock(&gStateMutex);
            *outSize = sizeof(UInt32); break;
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            CHECK_SIZE(sizeof(UInt32)); *(UInt32 *)outData = 1; *outSize = sizeof(UInt32); break;
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            CHECK_SIZE(sizeof(UInt32)); *(UInt32 *)outData = 0; *outSize = sizeof(UInt32); break;
        case kAudioDevicePropertyLatency:
            CHECK_SIZE(sizeof(UInt32)); *(UInt32 *)outData = 0; *outSize = sizeof(UInt32); break;
        case kAudioDevicePropertySafetyOffset:
            CHECK_SIZE(sizeof(UInt32));
            *(UInt32 *)outData = (a->mScope == kAudioObjectPropertyScopeInput) ? kInputSafetyOffset : 0;
            *outSize = sizeof(UInt32); break;
        case kAudioDevicePropertyIsHidden:
            CHECK_SIZE(sizeof(UInt32)); *(UInt32 *)outData = 0; *outSize = sizeof(UInt32); break;
        case kAudioDevicePropertyZeroTimeStampPeriod:
            CHECK_SIZE(sizeof(UInt32)); *(UInt32 *)outData = kZeroTimeStampPeriod; *outSize = sizeof(UInt32); break;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams: {
            AudioObjectID ids[2]; UInt32 n = 0;
            if (a->mScope == kAudioObjectPropertyScopeGlobal || a->mScope == kAudioObjectPropertyScopeInput)  ids[n++] = kObjectID_StreamIn;
            if (a->mScope == kAudioObjectPropertyScopeGlobal || a->mScope == kAudioObjectPropertyScopeOutput) ids[n++] = kObjectID_StreamOut;
            UInt32 fit = inDataSize / sizeof(AudioObjectID);
            if (n > fit) n = fit;
            memcpy(outData, ids, n * sizeof(AudioObjectID));
            *outSize = n * sizeof(AudioObjectID); break;
        }
        case kAudioObjectPropertyControlList:
            *outSize = 0; break;
        case kAudioDevicePropertyNominalSampleRate:
            CHECK_SIZE(sizeof(Float64));
            pthread_mutex_lock(&gStateMutex); *(Float64 *)outData = gSampleRate; pthread_mutex_unlock(&gStateMutex);
            *outSize = sizeof(Float64); break;
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            UInt32 n = inDataSize / sizeof(AudioValueRange);
            if (n > kNumSampleRates) n = kNumSampleRates;
            AudioValueRange *r = (AudioValueRange *)outData;
            for (UInt32 i = 0; i < n; i++) { r[i].mMinimum = kSampleRates[i]; r[i].mMaximum = kSampleRates[i]; }
            *outSize = n * sizeof(AudioValueRange); break;
        }
        case kAudioDevicePropertyPreferredChannelsForStereo:
            CHECK_SIZE(2 * sizeof(UInt32)); ((UInt32 *)outData)[0] = 1; ((UInt32 *)outData)[1] = 2; *outSize = 2 * sizeof(UInt32); break;
        default: return kAudioHardwareUnknownPropertyError;
        }
        return kAudioHardwareNoError;

    /* ---------------- streams */
    case kObjectID_StreamIn:
    case kObjectID_StreamOut: {
        Boolean isIn = (obj == kObjectID_StreamIn);
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
            CHECK_SIZE(sizeof(AudioClassID)); *(AudioClassID *)outData = kAudioObjectClassID; *outSize = sizeof(AudioClassID); break;
        case kAudioObjectPropertyClass:
            CHECK_SIZE(sizeof(AudioClassID)); *(AudioClassID *)outData = kAudioStreamClassID; *outSize = sizeof(AudioClassID); break;
        case kAudioObjectPropertyOwner:
            CHECK_SIZE(sizeof(AudioObjectID)); *(AudioObjectID *)outData = kObjectID_Device; *outSize = sizeof(AudioObjectID); break;
        case kAudioObjectPropertyOwnedObjects:
            *outSize = 0; break;
        case kAudioObjectPropertyName:
            CHECK_SIZE(sizeof(CFStringRef));
            *(CFStringRef *)outData = isIn ? CFSTR("ND-20 Network Inputs") : CFSTR("ND-20 Network Outputs");
            *outSize = sizeof(CFStringRef); break;
        case kAudioStreamPropertyIsActive:
            CHECK_SIZE(sizeof(UInt32));
            pthread_mutex_lock(&gStateMutex); *(UInt32 *)outData = isIn ? gStreamInActive : gStreamOutActive; pthread_mutex_unlock(&gStateMutex);
            *outSize = sizeof(UInt32); break;
        case kAudioStreamPropertyDirection:
            CHECK_SIZE(sizeof(UInt32)); *(UInt32 *)outData = isIn ? 1 : 0; *outSize = sizeof(UInt32); break;
        case kAudioStreamPropertyTerminalType:
            CHECK_SIZE(sizeof(UInt32));
            *(UInt32 *)outData = isIn ? kAudioStreamTerminalTypeLine : kAudioStreamTerminalTypeLine;
            *outSize = sizeof(UInt32); break;
        case kAudioStreamPropertyStartingChannel:
            CHECK_SIZE(sizeof(UInt32)); *(UInt32 *)outData = 1; *outSize = sizeof(UInt32); break;
        case kAudioStreamPropertyLatency:
            CHECK_SIZE(sizeof(UInt32)); *(UInt32 *)outData = 0; *outSize = sizeof(UInt32); break;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            CHECK_SIZE(sizeof(AudioStreamBasicDescription));
            pthread_mutex_lock(&gStateMutex);
            FillASBD((AudioStreamBasicDescription *)outData, ChannelsForStream(obj), gSampleRate);
            pthread_mutex_unlock(&gStateMutex);
            *outSize = sizeof(AudioStreamBasicDescription); break;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            UInt32 n = inDataSize / sizeof(AudioStreamRangedDescription);
            if (n > kNumSampleRates) n = kNumSampleRates;
            AudioStreamRangedDescription *r = (AudioStreamRangedDescription *)outData;
            for (UInt32 i = 0; i < n; i++) {
                FillASBD(&r[i].mFormat, ChannelsForStream(obj), kSampleRates[i]);
                r[i].mSampleRateRange.mMinimum = kSampleRates[i];
                r[i].mSampleRateRange.mMaximum = kSampleRates[i];
            }
            *outSize = n * sizeof(AudioStreamRangedDescription); break;
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
        return kAudioHardwareNoError;
    }
    }
    return kAudioHardwareBadObjectError;
}

/* ------------------------------------------------------- SetPropertyData */

static OSStatus RequestRate(Float64 rate) {
    Boolean ok = false;
    for (UInt32 i = 0; i < kNumSampleRates; i++) if (kSampleRates[i] == rate) ok = true;
    if (!ok) return kAudioHardwareIllegalOperationError;
    pthread_mutex_lock(&gStateMutex);
    Float64 current = gSampleRate;
    pthread_mutex_unlock(&gStateMutex);
    if (rate == current) return kAudioHardwareNoError;
    if (gHost) gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, (UInt64)rate, NULL);
    return kAudioHardwareNoError;
}

static OSStatus ND20_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                     UInt32 qSize, const void *qData, UInt32 inDataSize, const void *inData) {
    (void)qSize; (void)qData;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (a == NULL || inData == NULL) return kAudioHardwareIllegalOperationError;
    if (!ND20_HasProperty(d, obj, pid, a)) return kAudioHardwareUnknownPropertyError;

    if (obj == kObjectID_Device && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inDataSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        return RequestRate(*(const Float64 *)inData);
    }
    if (obj == kObjectID_StreamIn || obj == kObjectID_StreamOut) {
        switch (a->mSelector) {
        case kAudioStreamPropertyIsActive: {
            if (inDataSize != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            Boolean active = *(const UInt32 *)inData != 0;
            pthread_mutex_lock(&gStateMutex);
            if (obj == kObjectID_StreamIn) gStreamInActive = active; else gStreamOutActive = active;
            pthread_mutex_unlock(&gStateMutex);
            if (gHost) {
                AudioObjectPropertyAddress changed = {kAudioStreamPropertyIsActive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster};
                gHost->PropertiesChanged(gHost, obj, 1, &changed);
            }
            return kAudioHardwareNoError;
        }
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: {
            if (inDataSize != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
            const AudioStreamBasicDescription *f = (const AudioStreamBasicDescription *)inData;
            if (f->mFormatID != kAudioFormatLinearPCM || f->mChannelsPerFrame != ChannelsForStream(obj) ||
                f->mBitsPerChannel != 32 || !(f->mFormatFlags & kAudioFormatFlagIsFloat))
                return kAudioDeviceUnsupportedFormatError;
            return RequestRate(f->mSampleRate);
        }
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

/* ------------------------------------------------------------------- IO */

/* Starts the FireWire receiver and waits for the ND-20 clock. Called without gStateMutex held.
 * Returns true if the device clock can follow the ND-20. */
static Boolean StartReceiver(void) {
    gRx = nd20rx_start(kRxChannels, kNumRxChannels);
    if (!gRx) {
        RxLog("ND20Audio: no FireWire receive; running on the Mac clock with silent inputs");
        return false;
    }
    double refF = 0, refH = 0, tpf = 0, rate = 0;
    for (int waited = 0; waited < kLockWaitMs; waited += 10) {
        if (nd20rx_clock(gRx, &refF, &refH, &tpf, &rate)) break;
        usleep(10000);
    }
    if (!nd20rx_clock(gRx, &refF, &refH, &tpf, &rate)) {
        RxLog("ND20Audio: ND-20 stream did not lock in time; running on the Mac clock with silent inputs");
        nd20rx_stop(gRx);
        gRx = NULL;
        return false;
    }
    /* Device sample time 0 = the master frame arriving now. */
    double now = (double)mach_absolute_time();
    gFrameBase = floor(refF + (now - refH) / tpf);
    gZeroHost = (UInt64)(refH + (gFrameBase - refF) * tpf);

    char msg[200];
    snprintf(msg, sizeof msg, "ND20Audio: receiving, clock from channel %d at %.0f Hz",
             nd20rx_stream_channel(gRx, nd20rx_master_stream(gRx)), rate);
    RxLog(msg);
    if (rate != gSampleRate && (rate == 44100.0 || rate == 48000.0) && gHost) {
        snprintf(msg, sizeof msg, "ND20Audio: ND-20 runs at %.0f Hz but the device is set to %.0f Hz; asking to switch",
                 rate, gSampleRate);
        RxLog(msg);
        gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, (UInt64)rate, NULL);
    }
    return true;
}

static OSStatus ND20_StartIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client) {
    (void)client;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (id != kObjectID_Device) return kAudioHardwareBadDeviceError;
    pthread_mutex_lock(&gStateMutex);
    Boolean first = (gIOCount == 0);
    gIOCount++;
    pthread_mutex_unlock(&gStateMutex);
    if (!first) return kAudioHardwareNoError;

    Boolean rx = StartReceiver();         /* FIREWIRE: start receiving the ND-20 stream(s) */
    pthread_mutex_lock(&gStateMutex);
    gNumberTimeStamps = 0;
    gAnchorHostTime = mach_absolute_time();
    atomic_store(&gUseRx, rx ? 1 : 0);
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus ND20_StopIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client) {
    (void)client;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (id != kObjectID_Device) return kAudioHardwareBadDeviceError;
    pthread_mutex_lock(&gStateMutex);
    if (gIOCount > 0) gIOCount--;
    Boolean last = (gIOCount == 0);
    ND20Rx *rx = last ? gRx : NULL;
    if (last) { atomic_store(&gUseRx, 0); gRx = NULL; }
    pthread_mutex_unlock(&gStateMutex);
    if (rx) nd20rx_stop(rx);              /* FIREWIRE: stop the stream(s) */
    return kAudioHardwareNoError;
}

static OSStatus ND20_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client,
                                      Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed) {
    (void)client;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (id != kObjectID_Device) return kAudioHardwareBadDeviceError;
    pthread_mutex_lock(&gStateMutex);
    UInt64 now = mach_absolute_time();
    double refF, refH, tpf, rate;
    if (atomic_load(&gUseRx) && gRx && nd20rx_clock(gRx, &refF, &refH, &tpf, &rate)) {
        /* ND-20 clock: device frame S arrives at refH + (S + gFrameBase - refF) * tpf. */
        double current = refF + ((double)now - refH) / tpf - gFrameBase;
        double next = (double)(gNumberTimeStamps + 1) * kZeroTimeStampPeriod;
        if (current >= next) {
            gNumberTimeStamps++;
            UInt64 h = (UInt64)(refH + (next + gFrameBase - refF) * tpf);
            gZeroHost = (h > gZeroHost) ? h : gZeroHost + 1;
        }
        *outSampleTime = (Float64)(gNumberTimeStamps * kZeroTimeStampPeriod);
        *outHostTime   = gZeroHost;
    } else {
        Float64 ticksPerPeriod = gHostTicksPerFrame * (Float64)kZeroTimeStampPeriod;
        UInt64 nextTime = gAnchorHostTime + (UInt64)((Float64)(gNumberTimeStamps + 1) * ticksPerPeriod);
        if (nextTime <= now) gNumberTimeStamps++;
        *outSampleTime = (Float64)(gNumberTimeStamps * kZeroTimeStampPeriod);
        *outHostTime   = gAnchorHostTime + (UInt64)((Float64)gNumberTimeStamps * ticksPerPeriod);
    }
    *outSeed = 1;
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus ND20_WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op,
                                       Boolean *outWillDo, Boolean *outInPlace) {
    (void)client;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (id != kObjectID_Device) return kAudioHardwareBadDeviceError;
    Boolean will = (op == kAudioServerPlugInIOOperationReadInput || op == kAudioServerPlugInIOOperationWriteMix);
    if (outWillDo) *outWillDo = will;
    if (outInPlace) *outInPlace = true;
    return kAudioHardwareNoError;
}

static OSStatus ND20_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op, UInt32 frames,
                                      const AudioServerPlugInIOCycleInfo *info) {
    (void)client; (void)op; (void)frames; (void)info;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (id != kObjectID_Device) return kAudioHardwareBadDeviceError;
    return kAudioHardwareNoError;
}

static OSStatus ND20_DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, AudioObjectID stream, UInt32 client, UInt32 op,
                                   UInt32 frames, const AudioServerPlugInIOCycleInfo *info, void *mainBuf, void *secBuf) {
    (void)client; (void)info; (void)secBuf;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (id != kObjectID_Device) return kAudioHardwareBadDeviceError;
    if (op == kAudioServerPlugInIOOperationReadInput && stream == kObjectID_StreamIn && mainBuf) {
        /* FIREWIRE: copy received ND-20 samples (as float) into mainBuf. Routing N -> inputs 8N+1..8N+8. */
        memset(mainBuf, 0, (size_t)frames * ND20_INPUTS * kBytesPerSample);
        ND20Rx *rx = gRx;
        if (atomic_load(&gUseRx) && rx && info) {
            int64_t start = (int64_t)llround(info->mInputTime.mSampleTime + gFrameBase);
            for (int k = 0; k < nd20rx_stream_count(rx); k++) {
                int routing = nd20rx_stream_channel(rx, k);
                int firstCh = routing * 8;
                if (routing < 0 || firstCh >= ND20_INPUTS) continue;
                int64_t offset;
                if (!nd20rx_stream_offset(rx, k, &offset)) continue;   /* not aligned yet: silence */
                int nch = ND20_INPUTS - firstCh < 8 ? ND20_INPUTS - firstCh : 8;
                nd20rx_read_float(rx, k, start + offset, frames, nch, (float *)mainBuf + firstCh, ND20_INPUTS);
            }
        }
    } else if (op == kAudioServerPlugInIOOperationWriteMix && stream == kObjectID_StreamOut) {
        /* FIREWIRE HOOK (stage 3): queue mainBuf (float) for transmission to the ND-20 network. Discarded for now. */
    }
    return kAudioHardwareNoError;
}

static OSStatus ND20_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op, UInt32 frames,
                                    const AudioServerPlugInIOCycleInfo *info) {
    (void)client; (void)op; (void)frames; (void)info;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (id != kObjectID_Device) return kAudioHardwareBadDeviceError;
    return kAudioHardwareNoError;
}
