/*
 * nd20probe — read-only probe for the Otari ND-20 (mLAN / AV/C over FireWire).
 *
 * Asks the ND-20 to describe itself. It never writes to the device or changes any setting:
 *   1. Reads the full config ROM (macOS only caches part of it).
 *   2. Reads the IEC 61883-1 plug control registers (oMPR/oPCR, iMPR/iPCR).
 *   3. Sends AV/C STATUS queries: UNIT INFO, SUBUNIT INFO, PLUG INFO,
 *      plug signal formats, and stream-format queries.
 *
 * Build (on the Mac):  bash scripts/build-probe.sh
 * Run:                 ./build/nd20probe > ~/Desktop/nd20probe.txt
 */
#include <stdio.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <IOKit/avc/IOFireWireAVCLib.h>

#define OTARI_VENDOR_ID 0x000542

static void hexdump(const UInt8 *p, UInt32 n) {
    for (UInt32 i = 0; i < n; i++) printf("%02x ", p[i]);
    printf("\n");
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

/* ---------- Part 1 & 2: raw FireWire register reads ---------- */

static int read_q(IOFireWireLibDeviceRef dev, io_object_t svc, UInt32 lo, UInt32 *out) {
    FWAddress a;
    a.nodeID = 0; a.addressHi = 0xFFFF; a.addressLo = lo;
    UInt32 v = 0;
    IOReturn r = (*dev)->ReadQuadlet(dev, svc, &a, &v, 0 /* failOnReset */, 0);
    if (r != kIOReturnSuccess) return (int)r;
    *out = CFSwapInt32BigToHost(v);
    return 0;
}

static void probe_registers(io_object_t svc) {
    IOCFPlugInInterface **plug = NULL;
    SInt32 score = 0;
    IOReturn r = IOCreatePlugInInterfaceForService(svc, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    if (r != kIOReturnSuccess || !plug) { printf("!! FireWire plug-in failed: 0x%08x\n", r); return; }

    IOFireWireLibDeviceRef dev = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID), (LPVOID *)&dev);
    IODestroyPlugInInterface(plug);
    if (!dev) { printf("!! No FireWire device interface\n"); return; }

    r = (*dev)->Open(dev);
    if (r != kIOReturnSuccess) {
        printf("!! Open failed: 0x%08x (another driver may own the device)\n", r);
        (*dev)->Release(dev);
        return;
    }

    printf("\n== Full config ROM (0xFFFFF0000400) ==\n");
    UInt32 q = 0;
    for (UInt32 i = 0; i < 256; i++) {
        int e = read_q(dev, svc, 0xF0000400 + i * 4, &q);
        if (e) { printf("(stopped at quadlet %u, err 0x%08x)\n", i, e); break; }
        printf("%08x%s", q, (i % 8 == 7) ? "\n" : " ");
    }
    printf("\n");

    printf("\n== IEC 61883-1 plug control registers ==\n");
    struct { const char *name; UInt32 addr; } regs[] = {
        {"oMPR", 0xF0000900}, {"iMPR", 0xF0000980},
    };
    for (int k = 0; k < 2; k++) {
        UInt32 mpr = 0;
        int e = read_q(dev, svc, regs[k].addr, &mpr);
        if (e) { printf("%s: read err 0x%08x\n", regs[k].name, e); continue; }
        UInt32 nplugs = mpr & 0x1F;
        printf("%s = 0x%08x  (rate cap %u, plugs %u", regs[k].name, mpr, mpr >> 30, nplugs);
        if (k == 0) printf(", bcast ch %u", (mpr >> 24) & 0x3F);
        printf(")\n");
        for (UInt32 p = 0; p < nplugs && p < 31; p++) {
            UInt32 pcr = 0;
            e = read_q(dev, svc, regs[k].addr + 4 + p * 4, &pcr);
            if (e) { printf("  %cPCR[%u]: read err 0x%08x\n", k ? 'i' : 'o', p, e); continue; }
            printf("  %cPCR[%u] = 0x%08x  online %u, bcast %u, p2p %u, ch %u",
                   k ? 'i' : 'o', p, pcr, pcr >> 31, (pcr >> 30) & 1, (pcr >> 24) & 0x3F, (pcr >> 16) & 0x3F);
            if (k == 0) printf(", rate %u, overhead %u, payload %u quadlets", (pcr >> 14) & 3, (pcr >> 10) & 0xF, pcr & 0x3FF);
            printf("\n");
        }
    }

    (*dev)->Close(dev);
    (*dev)->Release(dev);
}

/* ---------- Part 3: AV/C status queries ---------- */

static void avc(IOFireWireAVCLibUnitInterface **u, const char *label, const UInt8 *cmd, UInt32 len) {
    UInt8 resp[512];
    UInt32 rlen = sizeof(resp);
    printf("\n-- %s\n   cmd:  ", label);
    hexdump(cmd, len);
    IOReturn r = (*u)->AVCCommand(u, cmd, len, resp, &rlen);
    if (r != kIOReturnSuccess) { printf("   err:  0x%08x\n", r); return; }
    const char *rc = "?";
    switch (resp[0] & 0x0F) {
        case 0x08: rc = "NOT IMPLEMENTED"; break;
        case 0x09: rc = "ACCEPTED"; break;
        case 0x0A: rc = "REJECTED"; break;
        case 0x0C: rc = "STABLE"; break;
        case 0x0F: rc = "INTERIM"; break;
    }
    printf("   resp: ");
    hexdump(resp, rlen);
    printf("   (%s)\n", rc);
}

static void probe_avc(io_object_t avcUnit) {
    IOCFPlugInInterface **plug = NULL;
    SInt32 score = 0;
    IOReturn r = IOCreatePlugInInterfaceForService(avcUnit, kIOFireWireAVCLibUnitTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    if (r != kIOReturnSuccess || !plug) { printf("!! AV/C plug-in failed: 0x%08x\n", r); return; }

    IOFireWireAVCLibUnitInterface **u = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOFireWireAVCLibUnitInterfaceID), (LPVOID *)&u);
    IODestroyPlugInInterface(plug);
    if (!u) { printf("!! No AV/C unit interface\n"); return; }

    r = (*u)->open(u);
    if (r != kIOReturnSuccess) printf("(AV/C open returned 0x%08x — trying commands anyway)\n", r);

    printf("\n== AV/C status queries ==\n");
    /* ctype 0x01 = STATUS. Subunit byte: 0xFF = unit; 0x08 = audio subunit id 0. */
    const UInt8 unit_info[]    = {0x01, 0xFF, 0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const UInt8 subunit_info[] = {0x01, 0xFF, 0x31, 0x07, 0xFF, 0xFF, 0xFF, 0xFF};
    const UInt8 plug_unit[]    = {0x01, 0xFF, 0x02, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    const UInt8 plug_audio[]   = {0x01, 0x08, 0x02, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    avc(u, "UNIT INFO", unit_info, sizeof unit_info);
    avc(u, "SUBUNIT INFO", subunit_info, sizeof subunit_info);
    avc(u, "PLUG INFO (unit)", plug_unit, sizeof plug_unit);
    avc(u, "PLUG INFO (audio subunit)", plug_audio, sizeof plug_audio);

    for (UInt8 p = 0; p < 4; p++) {
        char label[64];
        const UInt8 in_fmt[]  = {0x01, 0xFF, 0x19, p, 0xFF, 0xFF, 0xFF, 0xFF};
        const UInt8 out_fmt[] = {0x01, 0xFF, 0x18, p, 0xFF, 0xFF, 0xFF, 0xFF};
        snprintf(label, sizeof label, "INPUT PLUG SIGNAL FORMAT, iPCR %u", p);
        avc(u, label, in_fmt, sizeof in_fmt);
        snprintf(label, sizeof label, "OUTPUT PLUG SIGNAL FORMAT, oPCR %u", p);
        avc(u, label, out_fmt, sizeof out_fmt);
    }

    /* EXTENDED STREAM FORMAT INFORMATION (opcode 0xBF, subfunction 0xC0 = single),
       unit isochronous plug 0, input (0x00) and output (0x01) direction. */
    const UInt8 esf_in[]  = {0x01, 0xFF, 0xBF, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF};
    const UInt8 esf_out[] = {0x01, 0xFF, 0xBF, 0xC0, 0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF};
    avc(u, "EXTENDED STREAM FORMAT (unit iso input plug 0)", esf_in, sizeof esf_in);
    avc(u, "EXTENDED STREAM FORMAT (unit iso output plug 0)", esf_out, sizeof esf_out);

    /* Audio subunit: FUNCTION BLOCK / descriptor queries are left for a later probe. */

    (*u)->close(u);
    (*u)->Release(u);
}

/* ---------- main ---------- */

static io_object_t find_child(io_object_t parent, const char *cls) {
    io_iterator_t it = 0;
    io_object_t found = 0, o;
    if (IORegistryEntryCreateIterator(parent, kIOServicePlane, kIORegistryIterateRecursively, &it) != KERN_SUCCESS) return 0;
    while ((o = IOIteratorNext(it))) {
        if (!found && IOObjectConformsTo(o, cls)) { found = o; continue; }
        IOObjectRelease(o);
    }
    IOObjectRelease(it);
    return found;
}

int main(void) {
    printf("nd20probe 0.1 — read-only\n");
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching("IOFireWireDevice"), &it) != KERN_SUCCESS) {
        printf("!! Could not search the FireWire bus\n");
        return 1;
    }
    io_object_t d;
    int found = 0;
    while ((d = IOIteratorNext(it))) {
        UInt32 vendor = cfnum(d, "Vendor_ID");
        io_name_t nm = "";
        IORegistryEntryGetName(d, nm);
        printf("\nFireWire device: vendor 0x%06x  (%s)\n", vendor, nm);
        if (vendor != OTARI_VENDOR_ID) { IOObjectRelease(d); continue; }
        found = 1;
        printf("=> Otari device found\n");
        probe_registers(d);
        io_object_t avcUnit = find_child(d, "IOFireWireAVCUnit");
        if (avcUnit) { probe_avc(avcUnit); IOObjectRelease(avcUnit); }
        else printf("!! No IOFireWireAVCUnit under this device\n");
        IOObjectRelease(d);
    }
    IOObjectRelease(it);
    if (!found) printf("\n!! No Otari device on the bus. Is the ND-20 powered on and connected?\n");
    printf("\nDone.\n");
    return found ? 0 : 2;
}
