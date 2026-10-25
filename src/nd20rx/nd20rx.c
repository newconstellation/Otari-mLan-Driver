/*
 * nd20rx: receive-only IEC 61883-6 / AM824 engine for the Otari ND-20. See nd20rx.h.
 *
 * How it works
 *   - Opens the Mac's own FireWire node (IOFireWireLocalNode) through IOFireWireLib.
 *     If that fails it opens the ND-20 itself instead (vendor ID 0x000542).
 *   - For each channel: a ring of NuDCL "receive packet" commands (one packet per DCL, with the
 *     4-byte isochronous header), a "remote talker" port standing in for the ND-20, a local
 *     listener port, and an isochronous channel created with doIRM = false, so nothing is booked
 *     at the bus's resource manager (the ND-20 already owns the channel).
 *   - Every GROUP packets a callback decodes the packets: isoch header, CIP header
 *     (SID, DBS, DBC, FMT, FDF, SYT), then the AM824 quadlets (label byte + 24-bit sample).
 *   - Samples go into a ring per stream. The first stream that locks becomes the clock master;
 *     its arrival times give the sample clock. Other streams are aligned to it (coarsely by
 *     arrival time, then exactly by matching SYT presentation timestamps).
 *
 * All FireWire objects live on one private thread with its own CFRunLoop.
 */
#include "nd20rx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <IOKit/firewire/IOFireWireLibIsoch.h>

#if defined(kIOFireWireDeviceInterfaceID_v9)
#  define ND20_FW_IID kIOFireWireDeviceInterfaceID_v9
#elif defined(kIOFireWireDeviceInterfaceID_v8)
#  define ND20_FW_IID kIOFireWireDeviceInterfaceID_v8
#else
#  define ND20_FW_IID kIOFireWireDeviceInterfaceID_v7
#endif

#define OTARI_VENDOR_ID   0x000542u

#define NDCL        512u               /* receive DCLs per channel (= packets, 64 ms at 8000/s) */
#define GROUP       16u                /* packets per callback (2 ms) */
#define NGROUPS     (NDCL / GROUP)
#define RXLEN       2056u              /* bytes received per packet: 4 header + up to 2052 payload */
#define TS_OFFSET   0u                 /* time stamp quadlet, inside each slot */
#define PKT_OFFSET  16u                /* packet (header + payload) inside each slot */
#define STRIDE      2080u              /* bytes per slot (>= PKT_OFFSET + RXLEN, multiple of 16) */

#define RING        65536u             /* sample frames kept per stream (power of two) */
#define RING_MASK   (RING - 1u)
#define RING_GUARD  4096u              /* frames near the overwrite point treated as unavailable */

#define FIT_POINTS  512                /* callbacks used for the clock fit (about 1 s) */
#define LOCK_POINTS 100                /* callbacks before the clock is called locked */
#define ALIGN_POINTS 200               /* callbacks averaged for a stream's coarse offset */
#define SYT_KEEP    64

/* ------------------------------------------------------------------ logging */

static nd20rx_log_fn gLog = NULL;

void nd20rx_set_log(nd20rx_log_fn fn) { gLog = fn; }

static void logf_(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (gLog) gLog(buf);
    else fprintf(stderr, "%s\n", buf);
}

/* ------------------------------------------------------------------ types */

typedef struct Stream Stream;

typedef struct {
    Stream  *stream;
    uint32_t first;                    /* first DCL index of this group */
} Group;

typedef struct { uint16_t syt; int64_t frame; } SytEvent;

struct Stream {
    struct ND20Rx *rx;
    int      index;
    int      fwChannel;

    /* FireWire objects (rx thread only) */
    uint8_t                         *buf;
    vm_size_t                        bufSize;
    IOFireWireLibNuDCLPoolRef        pool;
    IOFireWireLibLocalIsochPortRef   local;
    IOFireWireLibRemoteIsochPortRef  remote;
    IOFireWireLibIsochChannelRef     channel;
    NuDCLRef                         dcls[NDCL];
    Group                            groups[NGROUPS];
    int                              started;

    /* decoder state (rx thread only) */
    int      haveDBC;
    uint8_t  nextDBC;
    SytEvent syt[SYT_KEEP];
    int      sytCount, sytHead;
    double   coarseSum;
    int      coarseN;

    /* samples */
    int32_t         *ring;             /* RING * ND20RX_MAX_SLOTS */
    _Atomic uint64_t written;

    /* alignment to the clock master (written by rx thread, read anywhere) */
    _Atomic int      aligned;
    _Atomic int64_t  offset;

    /* statistics (copied out under statsLock) */
    pthread_mutex_t  statsLock;
    ND20RxStats      st;

    /* clock fit points (master only, rx thread) */
    double   fitFrame[FIT_POINTS];
    double   fitHost[FIT_POINTS];
    int      fitCount, fitHead;
};

struct ND20Rx {
    int              count;
    Stream           s[ND20RX_MAX_STREAMS];

    pthread_t        thread;
    CFRunLoopRef     runLoop;
    _Atomic int      stopRequested;
    pthread_mutex_t  startLock;
    pthread_cond_t   startCond;
    int              startDone;
    int              startOK;

    io_object_t              service;
    IOCFPlugInInterface    **plug;
    IOFireWireLibDeviceRef   dev;
    int                      devOpen;

    double           ticksPerSecond;

    /* clock master + published clock (seqlock) */
    _Atomic int      master;           /* stream index, -1 = none yet */
    _Atomic uint32_t clockSeq;
    double           clkRefFrame, clkRefHost, clkTicksPerFrame, clkRate;
    _Atomic int      clockLocked;
};

/* ------------------------------------------------------------------ helpers */

static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint32_t le32(const uint8_t *p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static double rateFromFDF(uint8_t fdf) {
    switch (fdf & 0x07) {
        case 0: return 32000.0;
        case 1: return 44100.0;
        case 2: return 48000.0;
        case 3: return 88200.0;
        case 4: return 96000.0;
        case 5: return 176400.0;
        case 6: return 192000.0;
        default: return 0.0;
    }
}

static int sytInterval(double rate) {
    if (rate > 100000.0) return 32;
    if (rate > 50000.0)  return 16;
    return 8;
}

/* ------------------------------------------------------------------ clock (master stream) */

static void publishClock(struct ND20Rx *rx, double refFrame, double refHost, double tpf, double rate, int locked) {
    atomic_fetch_add_explicit(&rx->clockSeq, 1, memory_order_acq_rel);     /* odd: writing */
    rx->clkRefFrame = refFrame;
    rx->clkRefHost = refHost;
    rx->clkTicksPerFrame = tpf;
    rx->clkRate = rate;
    atomic_fetch_add_explicit(&rx->clockSeq, 1, memory_order_acq_rel);     /* even: done */
    atomic_store_explicit(&rx->clockLocked, locked, memory_order_release);
}

int nd20rx_clock(ND20Rx *rx, double *refFrame, double *refHost, double *ticksPerFrame, double *sampleRate) {
    if (!rx || !atomic_load_explicit(&rx->clockLocked, memory_order_acquire)) return 0;
    for (int tries = 0; tries < 100; tries++) {
        uint32_t a = atomic_load_explicit(&rx->clockSeq, memory_order_acquire);
        if (a & 1u) continue;
        double f = rx->clkRefFrame, h = rx->clkRefHost, t = rx->clkTicksPerFrame, r = rx->clkRate;
        atomic_thread_fence(memory_order_acquire);
        uint32_t b = atomic_load_explicit(&rx->clockSeq, memory_order_acquire);
        if (a == b) {
            if (refFrame) *refFrame = f;
            if (refHost) *refHost = h;
            if (ticksPerFrame) *ticksPerFrame = t;
            if (sampleRate) *sampleRate = r;
            return 1;
        }
    }
    return 0;
}

int nd20rx_master_stream(ND20Rx *rx) { return rx ? atomic_load(&rx->master) : -1; }

/* Least-squares fit of host time against frame count over the last FIT_POINTS callbacks. */
static void updateClock(Stream *s, uint64_t hostNow) {
    struct ND20Rx *rx = s->rx;
    double frame = (double)atomic_load_explicit(&s->written, memory_order_relaxed);
    s->fitFrame[s->fitHead] = frame;
    s->fitHost[s->fitHead] = (double)hostNow;
    s->fitHead = (s->fitHead + 1) % FIT_POINTS;
    if (s->fitCount < FIT_POINTS) s->fitCount++;
    if (s->fitCount < 32 || s->st.sampleRate <= 0.0) return;

    int n = s->fitCount;
    int oldest = (s->fitHead - n + FIT_POINTS) % FIT_POINTS;
    double x0 = s->fitFrame[oldest], y0 = s->fitHost[oldest];
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < n; i++) {
        int k = (oldest + i) % FIT_POINTS;
        double x = s->fitFrame[k] - x0, y = s->fitHost[k] - y0;
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    double den = n * sxx - sx * sx;
    if (den <= 0.0) return;
    double slope = (n * sxy - sx * sy) / den;
    double icpt = (sy - slope * sx) / n;
    double nominal = rx->ticksPerSecond / s->st.sampleRate;
    if (fabs(slope / nominal - 1.0) > 0.02) {           /* not plausible: wait */
        if (atomic_load(&rx->clockLocked)) logf_("nd20rx: clock fit out of range (%.6f of nominal)", slope / nominal);
        return;
    }
    double refFrame = frame;
    double refHost = y0 + icpt + slope * (frame - x0);
    int locked = s->fitCount >= LOCK_POINTS;
    if (locked && !atomic_load(&rx->clockLocked))
        logf_("nd20rx: clock locked to channel %d at %.0f Hz (measured %.2f Hz)",
              s->fwChannel, s->st.sampleRate, rx->ticksPerSecond / slope);
    publishClock(rx, refFrame, refHost, slope, s->st.sampleRate, locked);
}

/* ------------------------------------------------------------------ stream alignment */

static void addSyt(Stream *s, uint16_t syt, int64_t frame) {
    s->syt[s->sytHead].syt = syt;
    s->syt[s->sytHead].frame = frame;
    s->sytHead = (s->sytHead + 1) % SYT_KEEP;
    if (s->sytCount < SYT_KEEP) s->sytCount++;
}

static void updateAlignment(Stream *s, uint64_t hostNow) {
    struct ND20Rx *rx = s->rx;
    int m = atomic_load(&rx->master);
    if (m < 0 || atomic_load(&s->aligned)) return;
    if (m == s->index) {
        atomic_store(&s->offset, 0);
        atomic_store(&s->aligned, 1);
        pthread_mutex_lock(&s->statsLock);
        s->st.aligned = 1; s->st.offset = 0; s->st.alignedBySYT = 0;
        pthread_mutex_unlock(&s->statsLock);
        return;
    }
    double refF, refH, tpf, rate;
    if (!nd20rx_clock(rx, &refF, &refH, &tpf, &rate)) return;
    if (s->st.sampleRate != rate) return;             /* different rate: cannot align */

    /* Coarse: where was the master when this stream had written 'written' frames? */
    double masterNow = refF + ((double)hostNow - refH) / tpf;
    s->coarseSum += (double)atomic_load(&s->written) - masterNow;
    s->coarseN++;
    if (s->coarseN < ALIGN_POINTS) return;
    double coarse = s->coarseSum / s->coarseN;

    /* Exact: find a pair of SYT events (one per stream) with the same presentation time.
     * Equal SYT values repeat every 16 bus cycles, so pick the pairing closest to the coarse estimate. */
    Stream *ms = &rx->s[m];
    int bySYT = 0;
    double best = coarse;
    double period = rate / 500.0;                      /* frames per 16 bus cycles (SYT wraps) */
    double bestErr = 1e18;
    for (int a = 0; a < ms->sytCount; a++)
        for (int b = 0; b < s->sytCount; b++) {
            if (ms->syt[a].syt != s->syt[b].syt) continue;
            double c = (double)(s->syt[b].frame - ms->syt[a].frame);
            double err = fabs(c - coarse);
            if (err < bestErr) { bestErr = err; best = c; }
        }
    if (bestErr < period / 2.0) bySYT = 1;
    else {
        best = coarse;
        if (s->coarseN < 10 * ALIGN_POINTS) return;    /* keep trying to match SYT for a while */
    }
    int64_t off = (int64_t)llround(best);
    atomic_store(&s->offset, off);
    atomic_store(&s->aligned, 1);
    pthread_mutex_lock(&s->statsLock);
    s->st.aligned = 1; s->st.offset = off; s->st.alignedBySYT = bySYT;
    pthread_mutex_unlock(&s->statsLock);
    logf_("nd20rx: channel %d aligned to channel %d: offset %lld frames (%s, coarse estimate %.1f)",
          s->fwChannel, ms->fwChannel, (long long)off, bySYT ? "matched by SYT" : "arrival time only", coarse);
}

/* ------------------------------------------------------------------ packet decoding */

static void decodePacket(Stream *s, uint32_t dclIndex) {
    uint8_t *slot = s->buf + (size_t)dclIndex * STRIDE;
    uint8_t *p = slot + PKT_OFFSET;
    uint32_t ts = *(uint32_t *)(slot + TS_OFFSET);
    ND20RxStats *st = &s->st;

    st->packets++;
    st->lastTimestamp = ts;

    int captured = -1;
    if (st->rawCount < ND20RX_RAW_KEEP) {
        captured = st->rawCount++;
        memcpy(st->raw[captured], p, ND20RX_RAW_BYTES);
        st->rawLen[captured] = 0;
        st->rawTimestamp[captured] = ts;
    }

    /* Isochronous header: data_length(16) tag(2) channel(6) tcode(4) sy(4). Find its byte order. */
    uint32_t hb = be32(p), hl = le32(p), h = 0;
    int order = 0;
    if (((hb >> 4) & 0xF) == 0xA && (int)((hb >> 8) & 0x3F) == s->fwChannel) { h = hb; order = 1; }
    else if (((hl >> 4) & 0xF) == 0xA && (int)((hl >> 8) & 0x3F) == s->fwChannel) { h = hl; order = 2; }
    if (!order) {
        /* Maybe there is no header and the CIP starts right away (EOH1 bits = 10b, FMT 0x10)? */
        uint32_t q1 = be32(p + 4);
        if ((q1 >> 30) == 2u && ((q1 >> 24) & 0x3F) == 0x10) st->headerOrder = 3;
        st->badPackets++;
        return;
    }
    st->headerOrder = order;
    st->lastHeader = h;
    uint32_t len = h >> 16;
    if (captured >= 0) st->rawLen[captured] = len;
    if (len < 8 || len > RXLEN - 4) { st->badPackets++; return; }

    /* CIP header (always big-endian on the wire). */
    const uint8_t *cip = p + 4;
    uint32_t q0 = be32(cip), q1 = be32(cip + 4);
    st->lastCIP0 = q0; st->lastCIP1 = q1;
    if ((q0 >> 30) != 0u || (q1 >> 30) != 2u) { st->badPackets++; return; }
    uint8_t sid = (q0 >> 24) & 0x3F, dbs = (q0 >> 16) & 0xFF, dbc = q0 & 0xFF;
    uint8_t fmt = (q1 >> 24) & 0x3F, fdf = (q1 >> 16) & 0xFF;
    uint16_t syt = q1 & 0xFFFF;
    st->sid = sid; st->dbs = dbs; st->fmt = fmt; st->fdf = fdf; st->syt = syt;
    if (fmt != 0x10) { st->notAM824++; return; }

    uint32_t quads = (len - 8) / 4;
    uint32_t blocks = dbs ? quads / dbs : 0;
    if (blocks == 0 || fdf == 0xFF) { st->emptyPackets++; return; }

    double rate = rateFromFDF(fdf);
    if (rate > 0.0) st->sampleRate = rate;

    /* Lost packets show up as a jump in the data block counter. */
    if (s->haveDBC && dbc != s->nextDBC) st->dbcErrors++;
    s->nextDBC = (uint8_t)(dbc + blocks);
    s->haveDBC = 1;

    /* Audio slots: MBLA labels 0x40..0x4F; otherwise IEC 60958 labels 0x00..0x3F. */
    const uint8_t *data = cip + 8;
    int pos[ND20RX_MAX_SLOTS], n = 0;
    for (uint32_t q = 0; q < dbs && n < ND20RX_MAX_SLOTS; q++) {
        uint8_t label = data[q * 4];
        st->labelCount[label]++;
        if (label >= 0x40 && label <= 0x4F) pos[n++] = (int)q;
    }
    if (n == 0)
        for (uint32_t q = 0; q < dbs && n < ND20RX_MAX_SLOTS; q++)
            if (data[q * 4] < 0x40) pos[n++] = (int)q;
    st->audioSlots = n;
    for (int i = 0; i < n; i++) st->slotPos[i] = pos[i];

    st->dataPackets++;
    st->blocksLastPacket = blocks;
    if (st->minBlocks == 0 || blocks < st->minBlocks) st->minBlocks = blocks;
    if (blocks > st->maxBlocks) st->maxBlocks = blocks;

    uint64_t w = atomic_load_explicit(&s->written, memory_order_relaxed);

    /* SYT applies to the data block whose DBC is a multiple of the SYT interval. */
    if (syt != 0xFFFF && rate > 0.0) {
        int iv = sytInterval(rate);
        uint32_t idx = (uint32_t)((iv - (dbc % iv)) % iv);
        if (idx < blocks) addSyt(s, syt, (int64_t)(w + idx));
    }

    for (uint32_t b = 0; b < blocks; b++) {
        const uint8_t *blk = data + (size_t)b * dbs * 4;
        int32_t *dst = s->ring + (size_t)((w + b) & RING_MASK) * ND20RX_MAX_SLOTS;
        for (int i = 0; i < n; i++) {
            const uint8_t *q = blk + pos[i] * 4;
            int32_t v = (int32_t)(((uint32_t)q[1] << 24) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 8)) >> 8;
            dst[i] = v;
            float a = (float)(v < 0 ? -v : v) / 8388608.0f;
            if (a > st->peak[i]) st->peak[i] = a;
        }
        for (int i = n; i < ND20RX_MAX_SLOTS; i++) dst[i] = 0;
    }
    atomic_store_explicit(&s->written, w + blocks, memory_order_release);
    st->framesWritten = w + blocks;
}

static void groupCallback(void *refcon, NuDCLRef dcl) {
    (void)dcl;
    Group *g = (Group *)refcon;
    Stream *s = g->stream;
    struct ND20Rx *rx = s->rx;
    uint64_t now = mach_absolute_time();

    pthread_mutex_lock(&s->statsLock);
    for (uint32_t i = 0; i < GROUP; i++) decodePacket(s, g->first + i);
    s->st.callbacks++;
    pthread_mutex_unlock(&s->statsLock);

    if (s->st.dataPackets == 0) return;

    /* The first stream with audio becomes the clock master. */
    int expected = -1;
    if (atomic_load(&rx->master) < 0 && s->st.sampleRate > 0.0)
        if (atomic_compare_exchange_strong(&rx->master, &expected, s->index))
            logf_("nd20rx: channel %d is the clock master", s->fwChannel);
    if (atomic_load(&rx->master) == s->index) updateClock(s, now);
    updateAlignment(s, now);
}

/* ------------------------------------------------------------------ remote talker port */

static IOReturn remoteGetSupported(IOFireWireLibIsochPortRef port, IOFWSpeed *outMaxSpeed, UInt64 *outChans) {
    Stream *s = (Stream *)(*port)->GetRefCon(port);
    *outMaxSpeed = kFWSpeed400MBit;
    *outChans = ((UInt64)1) << (63 - s->fwChannel);    /* bit 63 = channel 0 */
    return kIOReturnSuccess;
}

/* ------------------------------------------------------------------ FireWire setup (rx thread) */

static int openFireWire(struct ND20Rx *rx) {
    const char *how = "Mac's FireWire node (IOFireWireLocalNode)";
    for (int attempt = 0; attempt < 2; attempt++) {
        io_object_t svc = 0;
        if (attempt == 0) {
            svc = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOFireWireLocalNode"));
        } else {
            how = "ND-20 device (IOFireWireDevice, vendor 0x000542)";
            io_iterator_t it = 0;
            if (IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching("IOFireWireDevice"), &it) == KERN_SUCCESS) {
                io_object_t o;
                while ((o = IOIteratorNext(it))) {
                    CFTypeRef v = IORegistryEntryCreateCFProperty(o, CFSTR("Vendor_ID"), NULL, 0);
                    UInt32 vid = 0;
                    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberSInt32Type, &vid);
                    if (v) CFRelease(v);
                    if (vid == OTARI_VENDOR_ID && !svc) svc = o;
                    else IOObjectRelease(o);
                }
                IOObjectRelease(it);
            }
        }
        if (!svc) { logf_("nd20rx: %s not found", how); continue; }

        IOCFPlugInInterface **plug = NULL;
        SInt32 score = 0;
        IOReturn r = IOCreatePlugInInterfaceForService(svc, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &plug, &score);
        if (r != kIOReturnSuccess || !plug) {
            logf_("nd20rx: %s: cannot load IOFireWireLib (0x%08x)", how, r);
            IOObjectRelease(svc);
            continue;
        }
        IOFireWireLibDeviceRef dev = NULL;
        (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(ND20_FW_IID), (LPVOID *)&dev);
        if (!dev) {
            logf_("nd20rx: %s: IOFireWireLib interface not available", how);
            IODestroyPlugInInterface(plug);
            IOObjectRelease(svc);
            continue;
        }
        r = (*dev)->Open(dev);
        if (r != kIOReturnSuccess) {
            logf_("nd20rx: %s: Open failed 0x%08x (is another program using it? quit Pro Tools / nd20listen)", how, r);
            (*dev)->Release(dev);
            IODestroyPlugInInterface(plug);
            IOObjectRelease(svc);
            continue;
        }
        rx->service = svc;
        rx->plug = plug;
        rx->dev = dev;
        rx->devOpen = 1;
        logf_("nd20rx: opened %s", how);
        return 1;
    }
    return 0;
}

static int setupStream(struct ND20Rx *rx, Stream *s) {
    IOFireWireLibDeviceRef dev = rx->dev;

    s->bufSize = (vm_size_t)NDCL * STRIDE;
    vm_address_t addr = 0;
    if (vm_allocate(mach_task_self(), &addr, s->bufSize, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        logf_("nd20rx: channel %d: out of memory", s->fwChannel);
        return 0;
    }
    s->buf = (uint8_t *)addr;
    memset(s->buf, 0, s->bufSize);

    s->pool = (*dev)->CreateNuDCLPool(dev, NDCL, CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));
    if (!s->pool) { logf_("nd20rx: channel %d: CreateNuDCLPool failed", s->fwChannel); return 0; }

    CFMutableSetRef groupSet = NULL;
    for (uint32_t i = 0; i < NDCL; i++) {
        if (i % GROUP == 0) groupSet = CFSetCreateMutable(kCFAllocatorDefault, 0, NULL);
        IOVirtualRange range;
        range.address = (IOVirtualAddress)(uintptr_t)(s->buf + (size_t)i * STRIDE + PKT_OFFSET);
        range.length = RXLEN;
        NuDCLRef dcl = (*s->pool)->AllocateReceivePacket(s->pool, groupSet, 4 /* isoch header bytes */, 1, &range);
        if (!dcl) {
            logf_("nd20rx: channel %d: AllocateReceivePacket failed at %u", s->fwChannel, i);
            if (groupSet) CFRelease(groupSet);
            return 0;
        }
        (*s->pool)->SetDCLTimeStampPtr(dcl, (UInt32 *)(void *)(s->buf + (size_t)i * STRIDE + TS_OFFSET));
        s->dcls[i] = dcl;
        if (i % GROUP == GROUP - 1) {
            Group *g = &s->groups[i / GROUP];
            g->stream = s;
            g->first = i - (GROUP - 1);
            (*s->pool)->SetDCLUpdateList(dcl, groupSet);       /* the pool keeps its own copy */
            (*s->pool)->SetDCLCallback(dcl, groupCallback);
            (*s->pool)->SetDCLRefcon(dcl, g);
            (*s->pool)->SetDCLFlags(dcl, kNuDCLUpdateBeforeCallback);
            CFRelease(groupSet);
            groupSet = NULL;
        }
    }
    (*s->pool)->SetDCLBranch(s->dcls[NDCL - 1], s->dcls[0]);   /* endless ring */

    IOVirtualRange bufRange;
    bufRange.address = (IOVirtualAddress)(uintptr_t)s->buf;
    bufRange.length = s->bufSize;
    s->local = (*dev)->CreateLocalIsochPort(dev, false /* listener */, (*s->pool)->GetProgram(s->pool),
                                            0, 0, 0, NULL, 0, &bufRange, 1,
                                            CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));
    if (!s->local) { logf_("nd20rx: channel %d: CreateLocalIsochPort failed", s->fwChannel); return 0; }

    s->remote = (*dev)->CreateRemoteIsochPort(dev, true /* the ND-20 is the talker */,
                                              CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));
    if (!s->remote) { logf_("nd20rx: channel %d: CreateRemoteIsochPort failed", s->fwChannel); return 0; }
    (*s->remote)->SetRefCon((IOFireWireLibIsochPortRef)s->remote, s);
    (*s->remote)->SetGetSupportedHandler(s->remote, remoteGetSupported);

    /* doIRM = false: listen only, book nothing. */
    s->channel = (*dev)->CreateIsochChannel(dev, false, 1024, kFWSpeed400MBit,
                                            CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));
    if (!s->channel) { logf_("nd20rx: channel %d: CreateIsochChannel failed", s->fwChannel); return 0; }

    IOReturn r = (*s->channel)->SetTalker(s->channel, (IOFireWireLibIsochPortRef)s->remote);
    if (!r) r = (*s->channel)->AddListener(s->channel, (IOFireWireLibIsochPortRef)s->local);
    if (r) { logf_("nd20rx: channel %d: SetTalker/AddListener failed 0x%08x", s->fwChannel, r); return 0; }
    r = (*s->channel)->AllocateChannel(s->channel);
    if (r) { logf_("nd20rx: channel %d: AllocateChannel failed 0x%08x", s->fwChannel, r); return 0; }
    r = (*s->channel)->Start(s->channel);
    if (r) {
        logf_("nd20rx: channel %d: Start failed 0x%08x", s->fwChannel, r);
        (*s->channel)->ReleaseChannel(s->channel);
        return 0;
    }
    s->started = 1;
    logf_("nd20rx: listening on isochronous channel %d", s->fwChannel);
    return 1;
}

static void teardownStream(struct ND20Rx *rx, Stream *s) {
    (void)rx;
    if (s->channel) {
        if (s->started) {
            (*s->channel)->Stop(s->channel);
            (*s->channel)->ReleaseChannel(s->channel);
        }
        (*s->channel)->Release(s->channel);
        s->channel = NULL;
    }
    s->started = 0;
    if (s->local)  { (*s->local)->Release(s->local);   s->local = NULL; }
    if (s->remote) { (*s->remote)->Release(s->remote); s->remote = NULL; }
    if (s->pool)   { (*s->pool)->Release(s->pool);     s->pool = NULL; }
    if (s->buf)    { vm_deallocate(mach_task_self(), (vm_address_t)s->buf, s->bufSize); s->buf = NULL; }
}

static void signalStart(struct ND20Rx *rx, int ok) {
    pthread_mutex_lock(&rx->startLock);
    rx->startDone = 1;
    rx->startOK = ok;
    pthread_cond_broadcast(&rx->startCond);
    pthread_mutex_unlock(&rx->startLock);
}

static void *rxThread(void *arg) {
    struct ND20Rx *rx = (struct ND20Rx *)arg;
    pthread_setname_np("ND-20 FireWire receive");
    rx->runLoop = CFRunLoopGetCurrent();

    int ok = openFireWire(rx);
    if (ok) {
        IOReturn r = (*rx->dev)->AddCallbackDispatcherToRunLoop(rx->dev, rx->runLoop);
        if (!r) r = (*rx->dev)->AddIsochCallbackDispatcherToRunLoop(rx->dev, rx->runLoop);
        if (r) { logf_("nd20rx: cannot attach to run loop (0x%08x)", r); ok = 0; }
    }
    int listening = 0;
    if (ok) for (int i = 0; i < rx->count; i++) listening += setupStream(rx, &rx->s[i]);
    if (ok && listening == 0) ok = 0;
    signalStart(rx, ok);

    if (ok)
        while (!atomic_load(&rx->stopRequested))
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);

    for (int i = 0; i < rx->count; i++) teardownStream(rx, &rx->s[i]);
    if (rx->dev) {
        (*rx->dev)->RemoveIsochCallbackDispatcherFromRunLoop(rx->dev);
        (*rx->dev)->RemoveCallbackDispatcherFromRunLoop(rx->dev);
        if (rx->devOpen) (*rx->dev)->Close(rx->dev);
        (*rx->dev)->Release(rx->dev);
        rx->dev = NULL;
    }
    if (rx->plug) { IODestroyPlugInInterface(rx->plug); rx->plug = NULL; }
    if (rx->service) { IOObjectRelease(rx->service); rx->service = 0; }
    return NULL;
}

/* ------------------------------------------------------------------ public API */

static void freeRx(struct ND20Rx *rx) {
    for (int i = 0; i < rx->count; i++) {
        free(rx->s[i].ring);
        pthread_mutex_destroy(&rx->s[i].statsLock);
    }
    pthread_mutex_destroy(&rx->startLock);
    pthread_cond_destroy(&rx->startCond);
    free(rx);
}

ND20Rx *nd20rx_start(const int *channels, int count) {
    if (count < 1 || count > ND20RX_MAX_STREAMS) { logf_("nd20rx: need 1..%d channels", ND20RX_MAX_STREAMS); return NULL; }
    struct ND20Rx *rx = calloc(1, sizeof *rx);
    if (!rx) return NULL;
    pthread_mutex_init(&rx->startLock, NULL);
    pthread_cond_init(&rx->startCond, NULL);
    atomic_init(&rx->master, -1);
    atomic_init(&rx->clockSeq, 0);
    atomic_init(&rx->clockLocked, 0);
    atomic_init(&rx->stopRequested, 0);

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    rx->ticksPerSecond = 1.0e9 * (double)tb.denom / (double)tb.numer;

    for (int i = 0; i < count; i++) {
        if (channels[i] < 0 || channels[i] > 63) { logf_("nd20rx: bad channel %d", channels[i]); rx->count = i; freeRx(rx); return NULL; }
        Stream *s = &rx->s[i];
        s->rx = rx;
        s->index = i;
        s->fwChannel = channels[i];
        s->st.fwChannel = channels[i];
        s->ring = calloc((size_t)RING * ND20RX_MAX_SLOTS, sizeof(int32_t));
        pthread_mutex_init(&s->statsLock, NULL);
        atomic_init(&s->written, 0);
        atomic_init(&s->aligned, 0);
        atomic_init(&s->offset, 0);
        rx->count = i + 1;
        if (!s->ring) { logf_("nd20rx: out of memory"); freeRx(rx); return NULL; }
    }

    if (pthread_create(&rx->thread, NULL, rxThread, rx) != 0) { logf_("nd20rx: cannot start thread"); freeRx(rx); return NULL; }
    pthread_mutex_lock(&rx->startLock);
    while (!rx->startDone) pthread_cond_wait(&rx->startCond, &rx->startLock);
    int ok = rx->startOK;
    pthread_mutex_unlock(&rx->startLock);
    if (!ok) {
        atomic_store(&rx->stopRequested, 1);
        pthread_join(rx->thread, NULL);
        freeRx(rx);
        return NULL;
    }
    return rx;
}

void nd20rx_stop(ND20Rx *rx) {
    if (!rx) return;
    atomic_store(&rx->stopRequested, 1);
    if (rx->runLoop) CFRunLoopWakeUp(rx->runLoop);
    pthread_join(rx->thread, NULL);
    freeRx(rx);
}

int nd20rx_stream_count(ND20Rx *rx) { return rx ? rx->count : 0; }

int nd20rx_stream_channel(ND20Rx *rx, int stream) {
    if (!rx || stream < 0 || stream >= rx->count) return -1;
    return rx->s[stream].fwChannel;
}

int nd20rx_get_stats(ND20Rx *rx, int stream, ND20RxStats *out, int resetPeaks) {
    if (!rx || stream < 0 || stream >= rx->count || !out) return 0;
    Stream *s = &rx->s[stream];
    pthread_mutex_lock(&s->statsLock);
    *out = s->st;
    if (resetPeaks) memset(s->st.peak, 0, sizeof s->st.peak);
    pthread_mutex_unlock(&s->statsLock);
    return 1;
}

uint64_t nd20rx_frames_written(ND20Rx *rx, int stream) {
    if (!rx || stream < 0 || stream >= rx->count) return 0;
    return atomic_load_explicit(&rx->s[stream].written, memory_order_acquire);
}

int nd20rx_buffer_activity(ND20Rx *rx, int stream) {
    if (!rx || stream < 0 || stream >= rx->count) return 0;
    Stream *s = &rx->s[stream];
    if (!s->buf) return 0;
    int n = 0;
    for (uint32_t i = 0; i < NDCL; i++) {
        const volatile uint32_t *q = (const volatile uint32_t *)(void *)(s->buf + (size_t)i * STRIDE + PKT_OFFSET);
        if (q[0] || q[1] || q[2]) n++;
    }
    return n;
}

int nd20rx_stream_offset(ND20Rx *rx, int stream, int64_t *offset) {
    if (!rx || stream < 0 || stream >= rx->count) return 0;
    Stream *s = &rx->s[stream];
    if (!atomic_load_explicit(&s->aligned, memory_order_acquire)) return 0;
    if (offset) *offset = atomic_load_explicit(&s->offset, memory_order_acquire);
    return 1;
}

/* Frames [lo, hi) are safe to read. */
static void readableRange(Stream *s, int64_t *lo, int64_t *hi) {
    int64_t w = (int64_t)atomic_load_explicit(&s->written, memory_order_acquire);
    *hi = w;
    *lo = w - (int64_t)(RING - RING_GUARD);
    if (*lo < 0) *lo = 0;
}

uint32_t nd20rx_read_float(ND20Rx *rx, int stream, int64_t first, uint32_t frames, int nch, float *dst, uint32_t dstStride) {
    if (!rx || stream < 0 || stream >= rx->count || !dst) return 0;
    if (nch > ND20RX_MAX_SLOTS) nch = ND20RX_MAX_SLOTS;
    Stream *s = &rx->s[stream];
    int64_t lo, hi;
    readableRange(s, &lo, &hi);
    uint32_t got = 0;
    const float scale = 1.0f / 8388608.0f;
    for (uint32_t f = 0; f < frames; f++) {
        int64_t fr = first + (int64_t)f;
        float *d = dst + (size_t)f * dstStride;
        if (fr < lo || fr >= hi) { for (int c = 0; c < nch; c++) d[c] = 0.0f; continue; }
        const int32_t *src = s->ring + (size_t)((uint64_t)fr & RING_MASK) * ND20RX_MAX_SLOTS;
        for (int c = 0; c < nch; c++) d[c] = (float)src[c] * scale;
        got++;
    }
    return got;
}

uint32_t nd20rx_read_int24(ND20Rx *rx, int stream, int64_t first, uint32_t frames, int nch, int32_t *dst) {
    if (!rx || stream < 0 || stream >= rx->count || !dst) return 0;
    if (nch > ND20RX_MAX_SLOTS) nch = ND20RX_MAX_SLOTS;
    Stream *s = &rx->s[stream];
    int64_t lo, hi;
    readableRange(s, &lo, &hi);
    uint32_t got = 0;
    for (uint32_t f = 0; f < frames; f++) {
        int64_t fr = first + (int64_t)f;
        int32_t *d = dst + (size_t)f * nch;
        if (fr < lo || fr >= hi) { for (int c = 0; c < nch; c++) d[c] = 0; continue; }
        const int32_t *src = s->ring + (size_t)((uint64_t)fr & RING_MASK) * ND20RX_MAX_SLOTS;
        for (int c = 0; c < nch; c++) d[c] = src[c];
        got++;
    }
    return got;
}
