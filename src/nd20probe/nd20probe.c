/*
 * nd20probe — read-only probe for the Otari ND-20 (mLAN / AV/C over FireWire).
 *
 * Asks the ND-20 to describe itself. It never writes to the device or changes any setting:
 *   1. Reads the full config ROM (macOS only caches part of it).
 *   1b. Dumps the mLAN register area named in the ROM's unit-dependent leaf (reads only).
 *   1c. Tries the reads Yamaha's mLAN Enabler 1.2 makes: block read of the address map at
 *       0xFFFFE0000000, and the NC1/PH1 ("NCP08") fixed registers at 0xFFFFED10xxxx.
 *   1d. Sweeps each space with 4-byte BLOCK reads (how Yamaha's HAL reads registers).
 *   AV/C queries only run with:  nd20probe --avc
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

static const char *errname(IOReturn r) {
    switch ((UInt32)r) {
        case 0xe0008017: return "address error (register not implemented)";
        case 0xe0008016: return "data error";
        case 0xe0008014: return "conflict error";
        case 0xe0008015: return "type error";
        case 0xe00002c0: return "no such device";
        case 0xe00002cd: return "device not open";
        case 0xe00002c5: return "exclusive access (another driver has it)";
        case 0xe00002d6: return "timeout";
        default: return "";
    }
}

/* ---------- Part 1 & 2: raw FireWire register reads ---------- */

static int read_q_hi(IOFireWireLibDeviceRef dev, io_object_t svc, UInt16 hi, UInt32 lo, UInt32 *out) {
    FWAddress a;
    a.nodeID = 0; a.addressHi = hi; a.addressLo = lo;
    UInt32 v = 0;
    IOReturn r = (*dev)->ReadQuadlet(dev, svc, &a, &v, 0 /* failOnReset */, 0);
    if (r != kIOReturnSuccess) return (int)r;
    *out = CFSwapInt32BigToHost(v);
    return 0;
}

static int read_q(IOFireWireLibDeviceRef dev, io_object_t svc, UInt32 lo, UInt32 *out) {
    return read_q_hi(dev, svc, 0xFFFF, lo, out);
}

static int read_block(IOFireWireLibDeviceRef dev, io_object_t svc, UInt16 hi, UInt32 lo, UInt8 *buf, UInt32 *size) {
    FWAddress a;
    a.nodeID = 0; a.addressHi = hi; a.addressLo = lo;
    IOReturn r = (*dev)->Read(dev, svc, &a, buf, size, 0, 0);
    return r == kIOReturnSuccess ? 0 : (int)r;
}

static void try_block(IOFireWireLibDeviceRef dev, io_object_t svc, const char *label, UInt16 hi, UInt32 lo, UInt32 len) {
    UInt8 buf[512];
    UInt32 size = len > sizeof buf ? sizeof buf : len;
    printf("%-34s 0x%04x%08x [%3u bytes, block]: ", label, hi, lo, size);
    int e = read_block(dev, svc, hi, lo, buf, &size);
    if (e) { printf("err 0x%08x %s\n", e, errname((IOReturn)e)); return; }
    for (UInt32 i = 0; i < size; i++) { if (i && i % 32 == 0) printf("\n%52s", ""); printf("%02x%s", buf[i], (i % 4 == 3) ? " " : ""); }
    printf("\n");
}

static void try_quad(IOFireWireLibDeviceRef dev, io_object_t svc, const char *label, UInt16 hi, UInt32 lo) {
    UInt32 v = 0;
    printf("%-34s 0x%04x%08x [quadlet]: ", label, hi, lo);
    int e = read_q_hi(dev, svc, hi, lo, &v);
    if (e) printf("err 0x%08x %s\n", e, errname((IOReturn)e));
    else printf("%08x\n", v);
}

/* Sweep with 4-byte block reads, 8 per line; "--------" = unreadable. */
static void sweep4(IOFireWireLibDeviceRef dev, io_object_t svc, UInt32 lo, UInt32 bytes) {
    UInt32 ok = 0, bad = 0, firstErr = 0;
    for (UInt32 off = 0; off < bytes; off += 4) {
        UInt8 b[4]; UInt32 sz = 4;
        if (off % 32 == 0) printf("  %04x%08x: ", 0xFFFF, lo + off);
        int e = read_block(dev, svc, 0xFFFF, lo + off, b, &sz);
        if (e || sz != 4) { printf("-------- "); if (!bad++) firstErr = (UInt32)e; }
        else { printf("%02x%02x%02x%02x ", b[0], b[1], b[2], b[3]); ok++; }
        if (off % 32 == 28) printf("\n");
    }
    if (bytes % 32) printf("\n");
    printf("  (%u readable, %u unreadable", ok, bad);
    if (bad) printf("; first error 0x%08x %s", firstErr, errname((IOReturn)firstErr));
    printf(")\n");
}

/* Dump 'count' quadlets from hi:lo, 8 per line, marking unreadable ones as "--------". */
__attribute__((unused)) static void dump_region(IOFireWireLibDeviceRef dev, io_object_t svc, UInt16 hi, UInt32 lo, UInt32 count) {
    UInt32 errs = 0, firstErr = 0;
    for (UInt32 i = 0; i < count; i++) {
        UInt32 v = 0;
        if (i % 8 == 0) printf("%04x%08x: ", hi, lo + i * 4);
        int e = read_q_hi(dev, svc, hi, lo + i * 4, &v);
        if (e) { printf("-------- "); if (!errs++) firstErr = (UInt32)e; }
        else printf("%08x ", v);
        if (i % 8 == 7) printf("\n");
    }
    if (errs) printf("(%u unreadable quadlets; first error 0x%08x %s)\n", errs, firstErr, errname((IOReturn)firstErr));
}

/* Find the unit-dependent (key 0x38) directory's address leaf in a config ROM image. */
static int find_mlan_base(const UInt32 *rom, UInt32 n, UInt16 *hi, UInt32 *lo) {
    if (n < 6) return -1;
    UInt32 root = 1 + (rom[0] >> 24);
    for (UInt32 k = 1; root + k < n && k <= (rom[root] >> 16); k++) {
        UInt32 e = rom[root + k];
        if ((e >> 24) != 0xD1) continue;                    /* unit directory */
        UInt32 ud = root + k + (e & 0xFFFFFF);
        for (UInt32 m = 1; ud + m < n && m <= (rom[ud] >> 16); m++) {
            UInt32 f = rom[ud + m];
            if ((f >> 30) != 3 || ((f >> 24) & 0x3F) != 0x38) continue;
            UInt32 dd = ud + m + (f & 0xFFFFFF);
            for (UInt32 j = 1; dd + j < n && j <= (rom[dd] >> 16); j++) {
                UInt32 g = rom[dd + j];
                if ((g >> 30) != 2) continue;
                UInt32 leaf = dd + j + (g & 0xFFFFFF);
                if (leaf + 2 < n && (rom[leaf] >> 16) >= 2) {
                    *hi = rom[leaf + 1] & 0xFFFF;
                    *lo = rom[leaf + 2];
                    return 0;
                }
            }
        }
    }
    return -1;
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
    UInt32 rom[64], nrom = 0, q = 0;
    for (UInt32 i = 0; i < 64; i++) {
        int e = read_q(dev, svc, 0xF0000400 + i * 4, &q);
        if (e) { printf("(stopped at quadlet %u, err 0x%08x)\n", i, e); break; }
        rom[nrom++] = q;
        printf("%08x%s", q, (i % 8 == 7) ? "\n" : " ");
    }
    printf("\n");

    UInt16 mhi = 0xFFFF; UInt32 mlo = 0xE0010000;
    if (find_mlan_base(rom, nrom, &mhi, &mlo) == 0) printf("\nmLAN register base from ROM: 0x%04x%08x\n", mhi, mlo);
    else printf("\nmLAN base not found in ROM; using default 0x%04x%08x\n", mhi, mlo);

    printf("\n== Yamaha mLAN Enabler 1.2 reads (NC1/PH1 \"NCP08\" devices) ==\n");
    try_block(dev, svc, "Address map (Enabler reads 32 B)", 0xFFFF, 0xE0000000, 32);
    try_quad (dev, svc, "Address map quadlet 0",           0xFFFF, 0xE0000000);
    try_block(dev, svc, "Firmware version",                0xFFFF, 0xED102000, 32);
    try_quad (dev, svc, "Firmware version quadlet",        0xFFFF, 0xED102000);
    try_quad (dev, svc, "PH1 Rx delay offset register",    0xFFFF, 0xED101000);
    try_quad (dev, svc, "PH1 Tx control register",         0xFFFF, 0xED101010);
    try_quad (dev, svc, "Error register",                  0xFFFF, 0xED101014);
    try_block(dev, svc, "mLAN leaf address (block)",       mhi, mlo, 32);
    {
        UInt8 map[32]; UInt32 sz = sizeof map;
        if (read_block(dev, svc, 0xFFFF, 0xE0000000, map, &sz) == 0 && sz == 32) {
            const char *names[4] = {"BootParam", "Core", "mLAN", "App"};
            printf("\nAddress map decoded (offsets from 0xFFFFE0000000):\n");
            for (int k = 0; k < 4; k++) {
                UInt32 off = (map[k*8] << 24) | (map[k*8+1] << 16) | (map[k*8+2] << 8) | map[k*8+3];
                UInt32 len = (map[k*8+4] << 24) | (map[k*8+5] << 16) | (map[k*8+6] << 8) | map[k*8+7];
                printf("  %-9s space: offset 0x%08x size 0x%08x\n", names[k], off, len);
                if (off != 0xFFFFFFFF && len) {
                    UInt32 cap = len > 0x500 ? 0x400 : len;
                    printf("  %s space, first 0x%x bytes (4-byte block reads):\n", names[k], cap);
                    sweep4(dev, svc, 0xE0000000 + off, cap);
                }
                if (k == 1 && off != 0xFFFFFFFF) {
                    try_block(dev, svc, "  Core space version (Core+0xC0FFC0)", 0xFFFF, 0xE0000000 + off + 0xC0FFC0, 4);
                    try_block(dev, svc, "  Core space version, 64 B", 0xFFFF, 0xE0000000 + off + 0xC0FFC0, 64);
                }
                if (k == 2 && off != 0xFFFFFFFF) {
                    UInt32 m = 0xE0000000 + off;
                    printf("  mLAN registers, 4-byte block vs quadlet reads:\n");
                    UInt32 regs[] = {0x00, 0x04, 0x08, 0x0C, 0x10, 0x20, 0x28, 0x100, 0x104, 0x108};
                    const char *rn[] = {"enabler addr hi", "enabler addr lo", "transporter mode", "bandwidth req 0",
                                        "channel req 0", "serial port data addr", "serial port data out",
                                        "identify", "posted-write partition", "(0x108)"};
                    for (int r = 0; r < 10; r++) {
                        char lab[64]; snprintf(lab, sizeof lab, "    +0x%03x %s", regs[r], rn[r]);
                        try_block(dev, svc, lab, 0xFFFF, m + regs[r], 4);
                        try_quad (dev, svc, lab, 0xFFFF, m + regs[r]);
                    }
                }
            }
        }
    }



    printf("\n== IEC 61883-1 plug control registers ==\n");
    struct { const char *name; UInt32 addr; } regs[] = {
        {"oMPR", 0xF0000900}, {"iMPR", 0xF0000980},
    };
    for (int k = 0; k < 2; k++) {
        UInt32 mpr = 0;
        int e = read_q(dev, svc, regs[k].addr, &mpr);
        if (e) { printf("%s: read err 0x%08x %s\n", regs[k].name, e, errname(e)); continue; }
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
    if (r != kIOReturnSuccess) { printf("   err:  0x%08x %s\n", r, errname(r)); return; }
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

/* Why does the AV/C library fail to open? Try opening the user client ourselves. */
static void avc_diag(io_object_t device) {
    printf("\n== AV/C connection diagnostics ==\n");
    const char *classes[] = {"IOFireWireAVCUnit", "IOFireWireAVCSubUnit", "IOFireWireAVCNub"};
    for (int c = 0; c < 3; c++) {
        io_iterator_t it = 0;
        io_object_t o;
        if (IORegistryEntryCreateIterator(device, kIOServicePlane, kIORegistryIterateRecursively, &it) != KERN_SUCCESS) continue;
        while ((o = IOIteratorNext(it))) {
            if (IOObjectConformsTo(o, classes[c])) {
                io_name_t cls = "";
                IOObjectGetClass(o, cls);
                const UInt32 types[] = {0, 1, 12};   /* 12 = kIOFireWireAVCLibConnection */
                for (int t = 0; t < 3; t++) {
                    UInt32 type = types[t];
                    io_connect_t conn = 0;
                    kern_return_t kr = IOServiceOpen(o, mach_task_self(), type, &conn);
                    printf("  IOServiceOpen(%s [%s], type %u): 0x%08x%s\n", classes[c], cls, type, kr, kr ? "" : " OK");
                    if (conn) IOServiceClose(conn);
                }
                UInt32 busy = 0;
                IOServiceGetBusyState(o, &busy);
                printf("    busy state %u\n", busy);
            }
            IOObjectRelease(o);
        }
        IOObjectRelease(it);
    }
}

static void probe_avc(io_object_t avcUnit, io_object_t device) {
    IOCFPlugInInterface **plug = NULL;
    SInt32 score = 0;
    IOReturn r = IOCreatePlugInInterfaceForService(avcUnit, kIOFireWireAVCLibUnitTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    if (r != kIOReturnSuccess || !plug) { printf("!! AV/C plug-in failed: 0x%08x\n", r); return; }

    IOFireWireAVCLibUnitInterface **u = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOFireWireAVCLibUnitInterfaceID), (LPVOID *)&u);
    IODestroyPlugInInterface(plug);
    if (!u) { printf("!! No AV/C unit interface\n"); return; }

    IOFireWireLibDeviceRef dev = NULL;
    r = (*u)->open(u);
    printf("AV/C open: 0x%08x %s\n", r, errname(r));
    if (r != kIOReturnSuccess) {
        /* Fallback: open the device ourselves and share that session with the AV/C library. */
        IOCFPlugInInterface **dplug = NULL;
        if (IOCreatePlugInInterfaceForService(device, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &dplug, &score) == kIOReturnSuccess && dplug) {
            (*dplug)->QueryInterface(dplug, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID), (LPVOID *)&dev);
            IODestroyPlugInInterface(dplug);
        }
        if (dev && (*dev)->Open(dev) == kIOReturnSuccess) {
            r = (*u)->openWithSessionRef(u, (*dev)->GetSessionRef(dev));
            printf("AV/C openWithSessionRef: 0x%08x %s\n", r, errname(r));
        } else printf("(could not open device for session fallback)\n");
    }

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
    if (dev) { (*dev)->Close(dev); (*dev)->Release(dev); }
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

int main(int argc, char **argv) {
    int want_avc = (argc > 1 && strcmp(argv[1], "--avc") == 0);
    printf("nd20probe 0.5 — read-only\n");
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
        io_object_t avcUnit = find_child(d, "IOFireWireAVCUnit");
        avc_diag(d);
        if (avcUnit) { if (want_avc) probe_avc(avcUnit, d); IOObjectRelease(avcUnit); }
        else printf("!! No IOFireWireAVCUnit under this device\n");
        probe_registers(d);
        IOObjectRelease(d);
    }
    IOObjectRelease(it);
    if (!found) printf("\n!! No Otari device on the bus. Is the ND-20 powered on and connected?\n");
    printf("\nDone.\n");
    return found ? 0 : 2;
}
