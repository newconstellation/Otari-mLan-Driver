/*
 * nd20rx: receive-only IEC 61883-6 / AM824 engine for the Otari ND-20 (IOFireWireLib, user space).
 *
 * Listens on one or more FireWire isochronous channels (ND-20 routing number N = channel N),
 * decodes the CIP header and AM824 audio quadlets, and keeps the samples in a ring buffer per
 * stream. Listening only: nothing is written to the ND-20 and no bus bandwidth is booked
 * (the channel is opened with IRM handling switched off).
 *
 * Used by both the nd20listen tool and the ND20Audio Core Audio driver.
 */
#ifndef ND20RX_H
#define ND20RX_H

#include <stdint.h>

#define ND20RX_MAX_STREAMS 16
#define ND20RX_MAX_SLOTS   16          /* audio channels kept per stream (an ND-20 module has 8) */
#define ND20RX_RAW_KEEP    3           /* first packets kept for diagnostics */
#define ND20RX_RAW_BYTES   112

typedef void (*nd20rx_log_fn)(const char *message);

typedef struct {
    int      fwChannel;                /* isochronous channel (= ND-20 routing number) */
    uint64_t callbacks;                /* groups of packets processed */
    uint64_t packets;                  /* all packets seen */
    uint64_t dataPackets;              /* packets that carried audio */
    uint64_t emptyPackets;             /* CIP-only / NO-DATA packets */
    uint64_t badPackets;               /* header or CIP not understood */
    uint64_t notAM824;                 /* CIP format other than AM824 (FMT 0x10) */
    uint64_t dbcErrors;                /* data block counter jumps (lost packets) */
    int      headerOrder;              /* isoch header byte order: 0 unknown, 1 big-endian, 2 little-endian, 3 missing */
    uint32_t lastHeader;               /* last isoch header quadlet (as decoded) */
    uint32_t lastTimestamp;            /* last DCL time stamp, raw */
    uint32_t lastCIP0, lastCIP1;       /* last CIP header quadlets */
    uint8_t  sid, dbs, fmt, fdf;       /* from the CIP header */
    uint16_t syt;
    uint32_t blocksLastPacket;         /* data blocks in the last data packet */
    uint32_t minBlocks, maxBlocks;     /* range of data blocks per data packet */
    double   sampleRate;               /* from FDF (0 = unknown) */
    int      audioSlots;               /* AM824 audio quadlets per data block */
    int      slotPos[ND20RX_MAX_SLOTS];/* position of each audio slot inside the data block */
    uint64_t labelCount[256];          /* label bytes seen in the first data block of each packet */
    float    peak[ND20RX_MAX_SLOTS];   /* peak level 0..1 since the last reset */
    uint64_t framesWritten;            /* sample frames decoded so far */
    int      aligned;                  /* 1 once the stream's offset to the clock master is known */
    int      alignedBySYT;             /* 1 if that offset was refined using SYT timestamps */
    int64_t  offset;                   /* frames, relative to the clock master stream */
    int      rawCount;
    uint32_t rawLen[ND20RX_RAW_KEEP];
    uint32_t rawTimestamp[ND20RX_RAW_KEEP];
    uint8_t  raw[ND20RX_RAW_KEEP][ND20RX_RAW_BYTES];   /* isoch header + start of payload, as in memory */
} ND20RxStats;

typedef struct ND20Rx ND20Rx;

/* Where log lines go (default: stderr). */
void     nd20rx_set_log(nd20rx_log_fn fn);

/* Opens FireWire, starts listening on the given channels. Returns NULL on failure (reason is logged). */
ND20Rx  *nd20rx_start(const int *channels, int count);
void     nd20rx_stop(ND20Rx *rx);

int      nd20rx_stream_count(ND20Rx *rx);
int      nd20rx_stream_channel(ND20Rx *rx, int stream);
int      nd20rx_get_stats(ND20Rx *rx, int stream, ND20RxStats *out, int resetPeaks);
uint64_t nd20rx_frames_written(ND20Rx *rx, int stream);

/* Copy frames [first, first+frames) of channels 0..nch-1 of a stream.
 * Float version: writes dst[f*dstStride + c] (range -1..1). Frames not available are set to 0.
 * Int version: writes 24-bit values (sign-extended) to dst[f*nch + c].
 * Return the number of frames that held real data. Safe to call from a real-time thread. */
uint32_t nd20rx_read_float(ND20Rx *rx, int stream, int64_t first, uint32_t frames, int nch, float *dst, uint32_t dstStride);
uint32_t nd20rx_read_int24(ND20Rx *rx, int stream, int64_t first, uint32_t frames, int nch, int32_t *dst);

/* Clock recovered from the clock master stream (the first stream that locks).
 * Master frame F arrives at host time  refHost + (F - refFrame) * ticksPerFrame.
 * Returns 1 when locked. Safe to call from a real-time thread. */
int      nd20rx_clock(ND20Rx *rx, double *refFrame, double *refHost, double *ticksPerFrame, double *sampleRate);
int      nd20rx_master_stream(ND20Rx *rx);

/* Frame offset of a stream relative to the clock master: stream frame = master frame + offset.
 * Returns 1 once known. Safe to call from a real-time thread. */
int      nd20rx_stream_offset(ND20Rx *rx, int stream, int64_t *offset);

#endif
