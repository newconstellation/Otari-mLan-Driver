/*
 * nd20remote: tools for working out the Otari CB-181 Remote Control Unit's FireWire protocol.
 *
 *   nd20remote scan             READ ONLY. Lists every FireWire node (ND-20s, CB-181, the Mac):
 *                               IDs, names, GUID, node ID, unit directories, and the config ROM.
 *
 *   nd20remote watch [secs]     The Mac logs any write sent TO IT in the ND-20 register ranges
 *                               (0xFFFF_E000_0000 ...), including broadcast writes. Reads and locks
 *                               to the Mac are refused. Nothing is sent to any other device.
 *                               Safe with the whole system connected.
 *
 *   nd20remote pose [secs]      DECOY. The Mac pretends to be an ND-20: it adds a unit directory with
 *                               the ND-20's IDs and answers the ND-20 register ranges with the values
 *                               read from the real unit. Every read, write and lock from the CB-181
 *                               is logged. DISCONNECT ALL ND-20s FIRST (only the Mac and the CB-181
 *                               on the bus), so the remote can't confuse the Mac with a real unit.
 *
 * Log lines are also written to ~/Desktop/nd20remote-<mode>.txt.
 *
 * Build (on the Mac):  bash scripts/build-probe.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <mach/mach_time.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>

#define VERSION "0.1"

#if defined(kIOFireWireDeviceInterfaceID_v9)
#  define FW_IID kIOFireWireDeviceInterfaceID_v9
#elif defined(kIOFireWireDeviceInterfaceID_v8)
#  define FW_IID kIOFireWireDeviceInterfaceID_v8
#else
#  define FW_IID kIOFireWireDeviceInterfaceID_v7
#endif

/* ------------------------------------------------------------------ logging */

static FILE *gLogFile = NULL;
static uint64_t gStart = 0;
static double gTicksPerMs = 1e6;

static void out(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fflush(stdout);
    if (gLogFile) { fputs(buf, gLogFile); fflush(gLogFile); }
}

static double nowMs(void) { return (double)(mach_absolute_time() - gStart) / gTicksPerMs; }

static void openLog(const char *mode) {
    const char *home = getenv("HOME");
    if (!home) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/Desktop/nd20remote-%s.txt", home, mode);
    gLogFile = fopen(path, "w");
    if (gLogFile) printf("(also saving to %s)\n", path);
}

/* ------------------------------------------------------------------ scan: registry dump */

static void printValue(CFTypeRef v, int indent);

static void printDictEntry(const void *key, const void *value, void *ctx) {
    int indent = (int)(intptr_t)ctx;
    char k[256] = "?";
    if (CFGetTypeID(key) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)key, k, sizeof k, kCFStringEncodingUTF8);
    out("%*s%s = ", indent, "", k);
    printValue((CFTypeRef)value, indent);
}

static void printValue(CFTypeRef v, int indent) {
    CFTypeID t = CFGetTypeID(v);
    if (t == CFNumberGetTypeID()) {
        long long n = 0;
        CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &n);
        out("0x%llx (%lld)\n", n, n);
    } else if (t == CFStringGetTypeID()) {
        char s[512] = "";
        CFStringGetCString((CFStringRef)v, s, sizeof s, kCFStringEncodingUTF8);
        out("\"%s\"\n", s);
    } else if (t == CFBooleanGetTypeID()) {
        out("%s\n", CFBooleanGetValue((CFBooleanRef)v) ? "Yes" : "No");
    } else if (t == CFDataGetTypeID()) {
        CFIndex n = CFDataGetLength((CFDataRef)v);
        const UInt8 *p = CFDataGetBytePtr((CFDataRef)v);
        out("<%ld bytes>", (long)n);
        for (CFIndex i = 0; i < n && i < 1024; i++) {
            if (i % 32 == 0) out("\n%*s  ", indent, "");
            out("%02x%s", p[i], (i % 4 == 3) ? " " : "");
        }
        out("\n");
    } else if (t == CFDictionaryGetTypeID()) {
        out("{\n");
        CFDictionaryApplyFunction((CFDictionaryRef)v, printDictEntry, (void *)(intptr_t)(indent + 4));
        out("%*s}\n", indent, "");
    } else if (t == CFArrayGetTypeID()) {
        out("[\n");
        for (CFIndex i = 0; i < CFArrayGetCount((CFArrayRef)v); i++) {
            out("%*s", indent + 4, "");
            printValue(CFArrayGetValueAtIndex((CFArrayRef)v, i), indent + 4);
        }
        out("%*s]\n", indent, "");
    } else {
        out("(other)\n");
    }
}

static void printEntry(io_registry_entry_t e, int indent) {
    io_name_t cls = "", name = "";
    IOObjectGetClass(e, cls);
    IORegistryEntryGetName(e, name);
    out("%*s[%s] %s\n", indent, "", cls, name);
    CFMutableDictionaryRef props = NULL;
    if (IORegistryEntryCreateCFProperties(e, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS && props) {
        CFDictionaryApplyFunction(props, printDictEntry, (void *)(intptr_t)(indent + 2));
        CFRelease(props);
    }
}

static void printChildren(io_registry_entry_t e, int indent, int depth) {
    if (depth > 3) return;
    io_iterator_t it = 0;
    if (IORegistryEntryGetChildIterator(e, kIOServicePlane, &it) != KERN_SUCCESS) return;
    io_object_t c;
    while ((c = IOIteratorNext(it))) {
        printEntry(c, indent);
        printChildren(c, indent + 4, depth + 1);
        IOObjectRelease(c);
    }
    IOObjectRelease(it);
}

/* Read up to 256 quadlets of config ROM from a device with 4-byte block reads. */
static void readRom(io_object_t dev) {
    IOCFPlugInInterface **plug = NULL;
    SInt32 score = 0;
    if (IOCreatePlugInInterfaceForService(dev, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &plug, &score) || !plug) {
        out("    (config ROM: cannot load IOFireWireLib)\n");
        return;
    }
    IOFireWireLibDeviceRef d = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(FW_IID), (LPVOID *)&d);
    if (!d) { IODestroyPlugInInterface(plug); return; }
    IOReturn r = (*d)->Open(d);
    if (r) {
        out("    (config ROM: cannot open device, 0x%08x; using the cached copy above)\n", r);
    } else {
        out("    Config ROM (live read, 4-byte block reads from 0xFFFFF0000400):\n");
        UInt32 n = 0;
        for (UInt32 i = 0; i < 256; i++) {
            UInt8 b[4];
            UInt32 sz = 4;
            FWAddress a;
            a.nodeID = 0; a.addressHi = 0xFFFF; a.addressLo = 0xF0000400 + i * 4;
            if ((*d)->Read(d, dev, &a, b, &sz, 0, 0) != kIOReturnSuccess || sz != 4) break;
            if (i % 8 == 0) out("      %3u: ", i);
            out("%02x%02x%02x%02x ", b[0], b[1], b[2], b[3]);
            if (i % 8 == 7) out("\n");
            n = i + 1;
        }
        if (n % 8) out("\n");
        out("      (%u quadlets read)\n", n);
        (*d)->Close(d);
    }
    (*d)->Release(d);
    IODestroyPlugInInterface(plug);
}

static int doScan(void) {
    out("nd20remote %s scan: READ ONLY\n\n", VERSION);
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching("IOFireWireLocalNode"), &it) == KERN_SUCCESS) {
        io_object_t o;
        while ((o = IOIteratorNext(it))) {
            out("=== This Mac's FireWire node ===\n");
            printEntry(o, 2);
            IOObjectRelease(o);
        }
        IOObjectRelease(it);
    }
    int count = 0;
    if (IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching("IOFireWireDevice"), &it) != KERN_SUCCESS) {
        out("!! cannot search the FireWire bus\n");
        return 1;
    }
    io_object_t d;
    while ((d = IOIteratorNext(it))) {
        count++;
        out("\n=== FireWire device %d ===\n", count);
        printEntry(d, 2);
        out("  Units / drivers attached:\n");
        printChildren(d, 4, 0);
        readRom(d);
        IOObjectRelease(d);
    }
    IOObjectRelease(it);
    out("\n%d FireWire device(s) besides this Mac.\nDone.\n", count);
    return 0;
}

/* ------------------------------------------------------------------ watch / pose: address spaces on the Mac */

typedef struct {
    const char *label;
    UInt32      baseLo;            /* address = 0xFFFF:baseLo */
    UInt32      size;
    UInt8      *store;             /* backing store: store[0..size) */
    IOFireWireLibPseudoAddressSpaceRef space;
} Window;

/* The ND-20 register ranges found so far (docs/nd20-config-rom.md). */
static Window gWin[] = {
    { "map/BootParam/mailbox", 0xE0000000u, 0x00100000u, NULL, NULL },   /* 0xFFFFE0000000 .. +1 MB */
    { "Core",                  0xE4000000u, 0x00010000u, NULL, NULL },
    { "mLAN",                  0xED000000u, 0x00010000u, NULL, NULL },
    { "NCP08 regs",            0xED100000u, 0x00010000u, NULL, NULL },
};
#define NWIN ((int)(sizeof gWin / sizeof gWin[0]))

static int  gPose = 0;
static int  gInLock = 0;           /* set while the library completes a lock (see onRead) */
static int  gPendingValid = 0;
static Window *gPendingWin = NULL;
static UInt32 gPendingOff = 0, gPendingLen = 0;
static UInt8  gPendingData[16];
static uint64_t gEvents = 0;
static volatile sig_atomic_t gQuit = 0;
static IOFireWireLibDeviceRef gDev = NULL;

static void onSignal(int s) { (void)s; gQuit = 1; }

static Window *winFor(IOFireWireLibPseudoAddressSpaceRef sp) {
    for (int i = 0; i < NWIN; i++) if (gWin[i].space == sp) return &gWin[i];
    return NULL;
}

static const char *regName(UInt32 lo) {
    if (lo >= 0xE0000000u && lo < 0xE0000020u) return "address map";
    if (lo >= 0xE0010000u && lo < 0xE0016D00u) return "BootParam (also the ROM's 0xFFFFE0010000 leaf)";
    if (lo >= 0xE000F000u && lo < 0xE0016D00u) return "BootParam";
    if (lo == 0xED000000u || lo == 0xED000004u) return "mLAN enabler address";
    if (lo == 0xED000008u) return "mLAN transporter mode";
    if (lo >= 0xED00000Cu && lo < 0xED000020u) return "mLAN bandwidth/channel request";
    if (lo >= 0xED000020u && lo < 0xED000030u) return "mLAN serial port";
    if (lo == 0xED000100u) return "mLAN identify";
    if (lo == 0xED000104u) return "mLAN posted-write partition";
    if (lo >= 0xED000000u && lo < 0xED00010Cu) return "mLAN space";
    if (lo >= 0xE4000000u && lo < 0xE4000500u) return "Core space";
    if (lo >= 0xED100000u && lo < 0xED110000u) return "NCP08 register";
    return "";
}

static void logPacket(const char *what, UInt16 src, UInt32 lo, UInt32 len, const UInt8 *data) {
    gEvents++;
    out("%10.1f ms  %-5s from node %04x  0xFFFF%08X  %3u bytes  %s\n", nowMs(), what, src, lo, len, regName(lo));
    if (data && len) {
        out("             ");
        for (UInt32 i = 0; i < len && i < 512; i++) {
            out("%02x%s", data[i], (i % 4 == 3) ? " " : "");
            if (i % 32 == 31 && i + 1 < len) out("\n             ");
        }
        if (len > 512) out("...");
        out("\n");
    }
}

/* Writes the library calls back (also used to deliver a lock's new value). */
static UInt32 onWrite(IOFireWireLibPseudoAddressSpaceRef sp, FWClientCommandID cmd, UInt32 len, void *packet,
                      UInt16 src, UInt32 hi, UInt32 lo, void *refcon) {
    (void)hi; (void)refcon;
    Window *w = winFor(sp);
    if (gInLock) {
        /* Lock compare matched: remember the new value, apply it after the response is sent. */
        if (w && len <= sizeof gPendingData) {
            gPendingValid = 1; gPendingWin = w; gPendingOff = lo - w->baseLo; gPendingLen = len;
            memcpy(gPendingData, packet, len);
        }
        return 0;   /* the library completes this command itself */
    }
    logPacket("WRITE", src, lo, len, (const UInt8 *)packet);
    if (w && lo - w->baseLo + len <= w->size) {
        memcpy(w->store + (lo - w->baseLo), packet, len);
        memcpy(w->store + lo, packet, len);                  /* mirror (see onRead) */
    }
    (*sp)->ClientCommandIsComplete(sp, cmd, kIOReturnSuccess);
    return 0;
}

/* Reads. IOFireWireLib also routes LOCK requests here, but with packetOffset = the absolute low address
 * (a quirk for fixed-address spaces), and then compares against store + absolute address. The store is
 * placed at the start of a 4 GB+ virtual reservation so that address is valid, and the value there is
 * kept as a mirror of the real register. */
static UInt32 onRead(IOFireWireLibPseudoAddressSpaceRef sp, FWClientCommandID cmd, UInt32 len, UInt32 offset,
                     UInt16 src, UInt32 hi, UInt32 lo, void *refcon) {
    (void)hi; (void)refcon;
    Window *w = winFor(sp);
    if (w && offset >= w->size) {
        UInt32 rel = lo - w->baseLo;
        UInt32 n = len / 2;                                   /* lock: arg + data, each len/2 */
        if (rel + n <= w->size) memcpy(w->store + lo, w->store + rel, n);
        out("%10.1f ms  LOCK  from node %04x  0xFFFF%08X  %3u-byte compare-swap  %s\n", nowMs(), src, lo, n, regName(lo));
        out("             current value: ");
        for (UInt32 i = 0; i < n && rel + i < w->size; i++) out("%02x%s", w->store[rel + i], (i % 4 == 3) ? " " : "");
        out("\n");
        gEvents++;
        gPendingValid = 0;
        gInLock = 1;
        (*sp)->ClientCommandIsComplete(sp, cmd, kIOReturnSuccess);
        gInLock = 0;
        if (gPendingValid) {
            out("             compare matched; new value: ");
            for (UInt32 i = 0; i < gPendingLen; i++) out("%02x%s", gPendingData[i], (i % 4 == 3) ? " " : "");
            out("\n");
            memcpy(gPendingWin->store + gPendingOff, gPendingData, gPendingLen);
            memcpy(gPendingWin->store + gPendingWin->baseLo + gPendingOff, gPendingData, gPendingLen);
            gPendingValid = 0;
        } else {
            out("             compare did not match; value unchanged\n");
        }
        return 0;
    }
    logPacket("READ", src, lo, len, NULL);
    if (w && offset + len <= w->size) {
        out("             answered: ");
        for (UInt32 i = 0; i < len && i < 64; i++) out("%02x%s", w->store[offset + i], (i % 4 == 3) ? " " : "");
        out("\n");
    }
    (*sp)->ClientCommandIsComplete(sp, cmd, kIOReturnSuccess);
    return 0;
}

static void onSkipped(IOFireWireLibPseudoAddressSpaceRef sp, FWClientCommandID cmd, UInt32 count) {
    out("%10.1f ms  !! %u packet(s) dropped (queue full)\n", nowMs(), count);
    (*sp)->ClientCommandIsComplete(sp, cmd, kIOReturnSuccess);
}

static void putBE32(UInt8 *p, UInt32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* Values read from the real ND-20 (nd20probe 0.3/0.4, nd20claim 0.2). */
static void preloadND20Image(void) {
    Window *w0 = &gWin[0], *wm = &gWin[2];
    static const UInt32 map[8] = { 0x0000f000, 0x00007d00, 0x04000000, 0x00000500,
                                   0x0d000000, 0x0000010c, 0x00000000, 0x00000000 };
    for (int i = 0; i < 8; i++) putBE32(w0->store + i * 4, map[i]);
    memset(w0->store + 0xF000, 0xFF, 0x7D00);                 /* BootParam: erased flash */
    putBE32(w0->store + 0xF000 + 0xF0, 0x0060ffff);
    memset(w0->store + 0xF000 + 0x300, 0x00, 0x100);          /* init table: zero */
    memset(wm->store, 0xFF, 2); memset(wm->store + 2, 0, 6);  /* enabler address: unowned */
    for (int i = 0; i < NWIN; i++) memcpy(gWin[i].store + gWin[i].baseLo, gWin[i].store, gWin[i].size);
}

static IOFireWireLibLocalUnitDirectoryRef gUnitDir = NULL;

static void publishUnitDirectory(void) {
    gUnitDir = (*gDev)->CreateLocalUnitDirectory(gDev, CFUUIDGetUUIDBytes(kIOFireWireLocalUnitDirectoryInterfaceID));
    if (!gUnitDir) { out("!! cannot create a unit directory; continuing without it\n"); return; }
    IOReturn r = 0;
    r |= (*gUnitDir)->AddEntry_UInt32(gUnitDir, kConfigUnitSpecIdKey, 0x00A02D, NULL);
    r |= (*gUnitDir)->AddEntry_UInt32(gUnitDir, kConfigUnitSwVersionKey, 0x010001, NULL);
    r |= (*gUnitDir)->AddEntry_UInt32(gUnitDir, kConfigModelIdKey, 0x000203, NULL);
    if (r == 0) r = (*gUnitDir)->Publish(gUnitDir);
    if (r) out("!! publishing the ND-20 unit directory failed (0x%08x); continuing without it\n", r);
    else out("Published a unit directory with the ND-20's IDs (spec 0x00A02D, version 0x010001, model 0x000203).\n"
             "Note: the Mac's root directory still says vendor Apple; the CB-181 may ignore the Mac because of that.\n");
}

static int doServe(int pose, int seconds) {
    gPose = pose;
    out("nd20remote %s %s: %s\n", VERSION, pose ? "pose" : "watch",
        pose ? "the Mac answers as an ND-20 (keep real ND-20s OFF the bus)"
             : "logging writes sent to the Mac; reads/locks refused; nothing is sent to other devices");

    io_object_t svc = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOFireWireLocalNode"));
    if (!svc) { out("!! the Mac's FireWire node was not found\n"); return 1; }
    IOCFPlugInInterface **plug = NULL;
    SInt32 score = 0;
    if (IOCreatePlugInInterfaceForService(svc, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &plug, &score) || !plug) {
        out("!! cannot load IOFireWireLib\n"); return 1;
    }
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(FW_IID), (LPVOID *)&gDev);
    if (!gDev) { out("!! IOFireWireLib interface missing\n"); return 1; }
    IOReturn r = (*gDev)->Open(gDev);
    if (r) { out("!! Open failed 0x%08x (quit Pro Tools / nd20listen and try again)\n", r); return 1; }
    (*gDev)->AddCallbackDispatcherToRunLoop(gDev, CFRunLoopGetCurrent());

    /* Each window's store sits at the start of a 4 GB + size virtual reservation (only touched pages use memory). */
    for (int i = 0; i < NWIN; i++) {
        size_t reserve = 0x100000000ull + gWin[i].size;
        void *p = mmap(NULL, reserve, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED) { out("!! cannot reserve memory\n"); return 1; }
        gWin[i].store = (UInt8 *)p;
    }
    if (pose) preloadND20Image();

    UInt32 flags = pose ? kFWAddressSpaceNoFlags : kFWAddressSpaceNoReadAccess;  /* NoRead also refuses locks */
    int ok = 0;
    for (int i = 0; i < NWIN; i++) {
        Window *w = &gWin[i];
        w->space = (*gDev)->CreateInitialUnitsPseudoAddressSpace(gDev, w->baseLo, w->size, w, 256 * 1024, w->store, flags,
                                                                CFUUIDGetUUIDBytes(kIOFireWirePseudoAddressSpaceInterfaceID));
        if (!w->space) { out("!! could not claim 0xFFFF%08X on the Mac (%s)\n", w->baseLo, w->label); continue; }
        (*w->space)->SetWriteHandler(w->space, onWrite);
        if (pose) (*w->space)->SetReadHandler(w->space, onRead);
        (*w->space)->SetSkippedPacketHandler(w->space, onSkipped);
        (*w->space)->TurnOnNotification(w->space);
        out("Listening at 0xFFFF%08X .. 0xFFFF%08X  (%s)\n", w->baseLo, w->baseLo + w->size - 1, w->label);
        ok++;
    }
    if (!ok) { out("!! nothing to listen on\n"); return 1; }
    if (pose) publishUnitDirectory();

    out("\nNow use the CB-181: power it up, change pages, turn GAIN, press PAD / +48V / LIMITER / ROUTING MONITOR.\n"
        "Write down the time (shown on the left) of each action so we can match it to the log.\n"
        "%s Ctrl-C to stop.\n\n", seconds > 0 ? "Stops by itself after the time given, or" : "");
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    double end = seconds > 0 ? nowMs() + seconds * 1000.0 : 1e300;
    double lastBeat = nowMs();
    while (!gQuit && nowMs() < end) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.2, false);
        if (nowMs() - lastBeat >= 10000.0) { out("%10.1f ms  (still listening, %llu events so far)\n", nowMs(), gEvents); lastBeat = nowMs(); }
    }

    out("\nStopping. %llu event(s) logged.\n", gEvents);
    if (gUnitDir) { (*gUnitDir)->Unpublish(gUnitDir); (*gUnitDir)->Release(gUnitDir); }
    for (int i = 0; i < NWIN; i++)
        if (gWin[i].space) { (*gWin[i].space)->TurnOffNotification(gWin[i].space); (*gWin[i].space)->Release(gWin[i].space); }
    (*gDev)->RemoveCallbackDispatcherFromRunLoop(gDev);
    (*gDev)->Close(gDev);
    (*gDev)->Release(gDev);
    IODestroyPlugInInterface(plug);
    IOObjectRelease(svc);
    out("Done.\n");
    return 0;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    gTicksPerMs = 1.0e6 * (double)tb.denom / (double)tb.numer;
    gStart = mach_absolute_time();

    const char *mode = argc > 1 ? argv[1] : "";
    int secs = argc > 2 ? atoi(argv[2]) : 0;
    if (!strcmp(mode, "scan")) { openLog("scan"); return doScan(); }
    if (!strcmp(mode, "watch")) { openLog("watch"); return doServe(0, secs); }
    if (!strcmp(mode, "pose")) {
        printf("POSE mode makes this Mac answer as an ND-20.\n"
               "Only the Mac and the CB-181 may be on the FireWire bus: unplug every ND-20 first.\n"
               "Type YES to continue: ");
        fflush(stdout);
        char line[16] = "";
        if (!fgets(line, sizeof line, stdin) || strncmp(line, "YES", 3) != 0) { printf("Cancelled.\n"); return 1; }
        openLog("pose");
        return doServe(1, secs);
    }
    printf("nd20remote %s\n"
           "  nd20remote scan            list every FireWire node and its config ROM (read only)\n"
           "  nd20remote watch [secs]    log writes sent to the Mac (safe with everything connected)\n"
           "  nd20remote pose  [secs]    the Mac pretends to be an ND-20 and logs the CB-181 (ND-20s unplugged)\n",
           VERSION);
    return 1;
}
