/*
 * nd20claim — claim the ND-20 the way Yamaha's mLAN Enabler does, read its control spaces,
 * then release the claim.
 *
 * The ONE write it performs (and then undoes):
 *   64-bit compare-and-swap on the mLAN "enabler address" register (mLAN space + 0x00,
 *   0xFFFF_ED00_0000 on the ND-20): 0xFFFF000000000000 ("unowned")  ->  our node + callback address.
 *   At the end it swaps it back to 0xFFFF000000000000. A power cycle also clears it.
 *
 * Build: bash scripts/build-probe.sh     Run: ./build/nd20claim > ~/Desktop/nd20claim.txt
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>

#define OTARI_VENDOR_ID 0x000542

/* Use the newest FireWire device interface the SDK offers (CompareSwap64 needs a recent one). */
#if defined(kIOFireWireDeviceInterfaceID_v9)
#  define FW_IID kIOFireWireDeviceInterfaceID_v9
#elif defined(kIOFireWireDeviceInterfaceID_v8)
#  define FW_IID kIOFireWireDeviceInterfaceID_v8
#elif defined(kIOFireWireDeviceInterfaceID_v7)
#  define FW_IID kIOFireWireDeviceInterfaceID_v7
#else
#  define FW_IID kIOFireWireDeviceInterfaceID_v4
#endif
#define UNOWNED_HI 0xFFFF0000u
#define UNOWNED_LO 0x00000000u

static IOFireWireLibDeviceRef gDev;
static io_object_t gSvc;

static const char *errname(IOReturn r) {
    switch ((UInt32)r) {
        case 0x00000000: return "ok";
        case 0xe0008017: return "address error (not implemented / not allowed)";
        case 0xe0008016: return "data error";
        case 0xe0008014: return "conflict/busy";
        case 0xe0008015: return "type error (transaction type not supported)";
        case 0xe00002c5: return "exclusive access (another driver has it)";
        case 0xe00002d6: return "timeout";
        default: return "";
    }
}

static int read4(UInt32 lo, UInt32 *out) {
    FWAddress a = {0, 0xFFFF, lo};
    UInt8 b[4]; UInt32 sz = 4;
    IOReturn r = (*gDev)->Read(gDev, gSvc, &a, b, &sz, 0, 0);
    if (r) return (int)r;
    if (sz != 4) return -1;
    *out = ((UInt32)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
    return 0;
}

static void sweep(const char *name, UInt32 lo, UInt32 bytes) {
    UInt32 ok = 0, bad = 0, firstErr = 0;
    printf("\n== %s (0xFFFF%08x, 0x%x bytes, 4-byte block reads) ==\n", name, lo, bytes);
    for (UInt32 off = 0; off < bytes; off += 4) {
        UInt32 v = 0;
        if (off % 32 == 0) printf("  +%04x: ", off);
        int e = read4(lo + off, &v);
        if (e) { printf("-------- "); if (!bad++) firstErr = (UInt32)e; } else { printf("%08x ", v); ok++; }
        if (off % 32 == 28) printf("\n");
    }
    if (bytes % 32) printf("\n");
    printf("  (%u readable, %u unreadable", ok, bad);
    if (bad) printf("; first error 0x%08x %s", firstErr, errname((IOReturn)firstErr));
    printf(")\n");
}

/* 64-bit lock (compare-and-swap). Values are host-order hi/lo; returns old value. */
static IOReturn cas64(UInt32 lo, UInt32 cmpHi, UInt32 cmpLo, UInt32 newHi, UInt32 newLo, UInt32 *oldHi, UInt32 *oldLo) {
    FWAddress a = {0, 0xFFFF, lo};
    UInt32 cmp[2] = {CFSwapInt32HostToBig(cmpHi), CFSwapInt32HostToBig(cmpLo)};
    UInt32 nv[2]  = {CFSwapInt32HostToBig(newHi), CFSwapInt32HostToBig(newLo)};
    UInt32 old[2] = {0, 0};
    IOReturn r = (*gDev)->CompareSwap64(gDev, gSvc, &a, cmp, nv, old, 8, 0, 0);
    *oldHi = CFSwapInt32BigToHost(old[0]);
    *oldLo = CFSwapInt32BigToHost(old[1]);
    return r;
}

static UInt32 cfnum(io_object_t obj, const char *key) {
    UInt32 v = 0xFFFFFFFF;
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef r = IORegistryEntryCreateCFProperty(obj, k, NULL, 0);
    if (r && CFGetTypeID(r) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)r, kCFNumberSInt32Type, &v);
    if (r) CFRelease(r);
    CFRelease(k);
    return v;
}

int main(int argc, char **argv) {
    int yes = (argc > 1 && strcmp(argv[1], "--yes") == 0);
    printf("nd20claim 0.1\n");
    if (!yes) {
        fprintf(stderr,
            "This tool makes ONE reversible write to the ND-20: it claims the device the way Yamaha's\n"
            "mLAN software did, reads its control registers, then releases the claim.\n"
            "Type YES and press Return to continue: ");
        char buf[16] = {0};
        if (!fgets(buf, sizeof buf, stdin) || strncmp(buf, "YES", 3) != 0) { fprintf(stderr, "Cancelled.\n"); return 1; }
    }

    io_iterator_t it = 0;
    IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching("IOFireWireDevice"), &it);
    io_object_t d;
    while ((d = IOIteratorNext(it))) {
        if (cfnum(d, "Vendor_ID") == OTARI_VENDOR_ID) { gSvc = d; break; }
        IOObjectRelease(d);
    }
    IOObjectRelease(it);
    if (!gSvc) { printf("!! No Otari device found. Is the ND-20 on and connected?\n"); return 2; }

    IOCFPlugInInterface **plug = NULL; SInt32 score = 0;
    if (IOCreatePlugInInterfaceForService(gSvc, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &plug, &score) || !plug) {
        printf("!! FireWire plug-in failed\n"); return 3;
    }
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(FW_IID), (LPVOID *)&gDev);
    IODestroyPlugInInterface(plug);
    if (!gDev) { printf("!! No FireWire device interface\n"); return 3; }
    IOReturn r = (*gDev)->Open(gDev);
    if (r) { printf("!! Open failed: 0x%08x %s\n", r, errname(r)); return 3; }

    /* Address map: tells us where the mLAN space is. */
    UInt8 map[32]; UInt32 msz = 32;
    FWAddress ma = {0, 0xFFFF, 0xE0000000};
    r = (*gDev)->Read(gDev, gSvc, &ma, map, &msz, 0, 0);
    if (r || msz != 32) { printf("!! Address map read failed: 0x%08x\n", r); (*gDev)->Close(gDev); return 4; }
    UInt32 sp[8];
    for (int i = 0; i < 8; i++) sp[i] = ((UInt32)map[i*4] << 24) | (map[i*4+1] << 16) | (map[i*4+2] << 8) | map[i*4+3];
    UInt32 coreLo = 0xE0000000 + sp[2], mlanLo = 0xE0000000 + sp[4];
    printf("Address map: BootParam +0x%x/0x%x  Core +0x%x/0x%x  mLAN +0x%x/0x%x\n", sp[0], sp[1], sp[2], sp[3], sp[4], sp[5]);

    /* Our side: a small address space the ND-20 can write notifications into. */
    static UInt8 backing[256];
    IOFireWireLibPseudoAddressSpaceRef pas = (*gDev)->CreatePseudoAddressSpace(gDev, sizeof backing, NULL, 0, backing,
        kFWAddressSpaceAutoWriteReply | kFWAddressSpaceAutoReadReply | kFWAddressSpaceAutoCopyOnWrite,
        CFUUIDGetUUIDBytes(kIOFireWirePseudoAddressSpaceInterfaceID));
    FWAddress cb = {0, 0, 0};
    if (pas) (*pas)->GetFWAddress(pas, &cb);
    UInt16 local = 0;
    (*gDev)->GetLocalNodeID(gDev, &local);
    UInt32 newHi = ((UInt32)local << 16) | cb.addressHi, newLo = cb.addressLo;
    printf("Our node 0x%04x, callback address 0x%04x%08x%s\n", local, cb.addressHi, cb.addressLo, pas ? "" : " (allocation FAILED)");

    printf("\n== Before claiming ==\n");
    UInt32 v = 0; int e = read4(mlanLo, &v);
    printf("  enabler address hi read: %s%08x\n", e ? "err " : "", e ? (UInt32)e : v);

    UInt32 oh = 0, ol = 0;
    r = cas64(mlanLo, UNOWNED_HI, UNOWNED_LO, newHi, newLo, &oh, &ol);
    printf("\n== CLAIM: lock 0xFFFF%08x  0x%08x%08x -> 0x%08x%08x ==\n", mlanLo, UNOWNED_HI, UNOWNED_LO, newHi, newLo);
    printf("  result 0x%08x %s; previous value 0x%08x%08x\n", r, errname(r), oh, ol);
    int claimed = (r == 0 && oh == UNOWNED_HI && ol == UNOWNED_LO);
    if (r == 0 && !claimed) printf("  !! Not claimed: the ND-20 already had an owner (value above). Not overriding it.\n");

    usleep(200000);
    sweep("mLAN space", mlanLo, sp[5] > 0x200 ? 0x200 : sp[5]);
    sweep("Core space", coreLo, sp[3] > 0x500 ? 0x500 : sp[3]);
    e = read4(coreLo + 0xC0FFC0, &v);
    printf("\nCore space version (Core+0xC0FFC0): %s%08x\n", e ? "err " : "", e ? (UInt32)e : v);
    e = read4(0xED102000, &v);
    printf("Firmware version (0xFFFFED102000): %s%08x\n", e ? "err " : "", e ? (UInt32)e : v);
    printf("Callback area (first 32 bytes written to us): ");
    for (int i = 0; i < 32; i++) printf("%02x%s", backing[i], (i % 4 == 3) ? " " : "");
    printf("\n");

    if (claimed) {
        r = cas64(mlanLo, newHi, newLo, UNOWNED_HI, UNOWNED_LO, &oh, &ol);
        printf("\n== RELEASE: lock back to 0x%08x%08x: result 0x%08x %s; previous 0x%08x%08x ==\n",
               UNOWNED_HI, UNOWNED_LO, r, errname(r), oh, ol);
        if (r) printf("  !! Release failed. Power-cycle the ND-20 to clear the claim.\n");
    }

    if (pas) (*pas)->Release(pas);
    (*gDev)->Close(gDev);
    (*gDev)->Release(gDev);
    IOObjectRelease(gSvc);
    printf("\nDone.\n");
    return 0;
}
