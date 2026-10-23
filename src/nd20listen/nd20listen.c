/*
 * nd20listen: listen to the ND-20's isochronous audio streams and describe them. Receive only.
 *
 * Shows, for each channel: packet counts, the CIP header (SID, DBS, FMT, FDF, SYT), sample rate,
 * number of audio channels, AM824 labels, lost packets, and a level meter per audio channel.
 * Optionally records each stream to a 24-bit WAV file so you can listen to what arrived.
 *
 * Build (on the Mac):  bash scripts/build-probe.sh
 * Run:                 ./build/nd20listen                  (channels 0 and 1, 10 seconds)
 *                      ./build/nd20listen -c 0,1 -t 30
 *                      ./build/nd20listen -w ~/Desktop/nd20test   (also writes nd20test_ch0.wav, ...)
 * Quit Pro Tools (or anything using "Otari ND-20 Network") first: only one program can listen at a time.
 */
#include "../nd20rx/nd20rx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <math.h>

#define VERSION "0.1"

static volatile sig_atomic_t gQuit = 0;
static void onSignal(int sig) { (void)sig; gQuit = 1; }

static void logLine(const char *m) { printf("  %s\n", m); fflush(stdout); }

/* ---------------------------------------------------------------- WAV writer (24-bit PCM) */

typedef struct {
    FILE    *f;
    int      nch;
    uint32_t rate;
    uint64_t frames;
    int64_t  next;          /* next stream frame to write */
    char     path[1024];
} Wav;

static void put16(FILE *f, uint16_t v) { fputc(v & 0xFF, f); fputc(v >> 8, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

static void wavHeader(Wav *w) {
    uint32_t dataBytes = (uint32_t)(w->frames * (uint64_t)w->nch * 3u);
    fseek(w->f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, w->f); put32(w->f, 36 + dataBytes);
    fwrite("WAVEfmt ", 1, 8, w->f); put32(w->f, 16);
    put16(w->f, 1); put16(w->f, (uint16_t)w->nch); put32(w->f, w->rate);
    put32(w->f, w->rate * (uint32_t)w->nch * 3u); put16(w->f, (uint16_t)(w->nch * 3)); put16(w->f, 24);
    fwrite("data", 1, 4, w->f); put32(w->f, dataBytes);
    fseek(w->f, 0, SEEK_END);
}

static void wavPump(ND20Rx *rx, int stream, Wav *w) {
    if (!w->f) return;
    int64_t written = (int64_t)nd20rx_frames_written(rx, stream);
    if (w->next < 0) w->next = written;                      /* start from "now" */
    if (written - w->next > 60000) w->next = written - 60000; /* fell behind: skip ahead */
    static int32_t tmp[4096 * 16];
    while (w->next < written) {
        uint32_t n = (uint32_t)(written - w->next);
        if (n > 4096) n = 4096;
        nd20rx_read_int24(rx, stream, w->next, n, w->nch, tmp);
        for (uint32_t i = 0; i < n * (uint32_t)w->nch; i++) {
            int32_t v = tmp[i];
            fputc(v & 0xFF, w->f); fputc((v >> 8) & 0xFF, w->f); fputc((v >> 16) & 0xFF, w->f);
        }
        w->frames += n;
        w->next += n;
    }
}

/* ---------------------------------------------------------------- printing */

static const char *orderName(int o) {
    switch (o) {
        case 1: return "big-endian";
        case 2: return "little-endian";
        case 3: return "MISSING (buffer starts with the CIP header)";
        default: return "not recognised";
    }
}

static const char *labelName(int l) {
    if (l >= 0x40 && l <= 0x43) return "MBLA audio (24-bit)";
    if (l >= 0x44 && l <= 0x4F) return "MBLA audio (other)";
    if (l < 0x40) return "IEC 60958";
    if (l >= 0x80 && l <= 0x83) return "MIDI";
    if (l >= 0x80 && l <= 0x8F) return "MIDI (other)";
    if (l >= 0xA0 && l <= 0xAF) return "high-precision MBLA";
    if (l >= 0xC0 && l <= 0xCF) return "SMPTE / sample count / ancillary";
    return "other";
}

static void meter(float peak, char *out, size_t len) {
    double db = peak > 0.0f ? 20.0 * log10(peak) : -200.0;
    int bars = db <= -60.0 ? 0 : (int)((db + 60.0) / 3.0 + 0.5);
    if (bars > 20) bars = 20;
    char bar[24];
    for (int i = 0; i < 20; i++) bar[i] = i < bars ? '#' : '.';
    bar[20] = 0;
    if (db <= -144.0) snprintf(out, len, "[%s]   silent", bar);
    else snprintf(out, len, "[%s] %6.1f dBFS%s", bar, db, peak >= 0.999f ? " CLIP" : "");
}

static void printFirstPackets(const ND20RxStats *s) {
    printf("\n  First packets on channel %d (as stored in memory; first quadlet should be the isoch header):\n", s->fwChannel);
    for (int i = 0; i < s->rawCount; i++) {
        printf("   #%d  timestamp %08x  length %u\n       ", i, s->rawTimestamp[i], s->rawLen[i]);
        for (int b = 0; b < ND20RX_RAW_BYTES; b++) {
            printf("%02x", s->raw[i][b]);
            if (b % 4 == 3) printf(" ");
            if (b % 32 == 31 && b + 1 < ND20RX_RAW_BYTES) printf("\n       ");
        }
        printf("\n");
    }
}

static void printDetail(const ND20RxStats *s) {
    printf("\n  Channel %d (ND-20 routing %d)\n", s->fwChannel, s->fwChannel);
    if (s->packets == 0) {
        printf("    No packets at all. Nothing is being sent on this channel (or reception failed).\n");
        return;
    }
    printf("    isoch header byte order : %s   last header %08x\n", orderName(s->headerOrder), s->lastHeader);
    printf("    CIP header              : %08x %08x\n", s->lastCIP0, s->lastCIP1);
    printf("      SID (source node)     : %u\n", s->sid);
    printf("      DBS (quadlets/block)  : %u\n", s->dbs);
    printf("      FMT                   : 0x%02x %s\n", s->fmt, s->fmt == 0x10 ? "(AM824 audio & music)" : "(NOT AM824)");
    printf("      FDF                   : 0x%02x %s\n", s->fdf, s->fdf == 0xFF ? "(NO-DATA)" : "");
    printf("      SYT (last)            : 0x%04x\n", s->syt);
    if (s->sampleRate > 0) printf("    sample rate             : %.0f Hz\n", s->sampleRate);
    else printf("    sample rate             : unknown\n");
    printf("    data blocks per packet  : %u..%u (last %u)\n", s->minBlocks, s->maxBlocks, s->blocksLastPacket);
    printf("    audio channels          : %d   (positions in each block:", s->audioSlots);
    for (int i = 0; i < s->audioSlots; i++) printf(" %d", s->slotPos[i]);
    printf(")\n");
    printf("    labels seen             :");
    int any = 0;
    for (int l = 0; l < 256; l++)
        if (s->labelCount[l]) { printf("%s 0x%02x %s", any ? "," : "", l, labelName(l)); any = 1; }
    printf("%s\n", any ? "" : " none yet");
}

/* ---------------------------------------------------------------- main */

static int parseChannels(const char *arg, int *ch, int max) {
    int n = 0;
    const char *p = arg;
    while (*p && n < max) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p || v < 0 || v > 63) return -1;
        ch[n++] = (int)v;
        p = end;
        if (*p == ',' || *p == ' ') p++;
    }
    return n;
}

static void usage(void) {
    printf("usage: nd20listen [-c 0,1] [-t seconds] [-w file_prefix]\n"
           "  -c  isochronous channels to listen on (ND-20 routing numbers). Default 0,1\n"
           "  -t  seconds to listen (default 10; 0 = until Ctrl-C)\n"
           "  -w  also record each channel to <prefix>_ch<N>.wav (24-bit)\n");
}

int main(int argc, char **argv) {
    int channels[ND20RX_MAX_STREAMS] = {0, 1};
    int nch = 2;
    int seconds = 10;
    const char *wavPrefix = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            nch = parseChannels(argv[++i], channels, ND20RX_MAX_STREAMS);
            if (nch < 1) { usage(); return 1; }
        } else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            seconds = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-w") && i + 1 < argc) {
            wavPrefix = argv[++i];
        } else { usage(); return 1; }
    }

    printf("nd20listen %s: receive only (nothing is written to the ND-20, no bandwidth is booked)\n", VERSION);
    printf("Listening on channel(s):");
    for (int i = 0; i < nch; i++) printf(" %d", channels[i]);
    printf("   for %s\n\n", seconds > 0 ? "the time given" : "ever (Ctrl-C to stop)");

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    nd20rx_set_log(logLine);

    ND20Rx *rx = nd20rx_start(channels, nch);
    if (!rx) {
        printf("\n!! Could not start listening. See the messages above.\n");
        return 2;
    }

    Wav wav[ND20RX_MAX_STREAMS];
    memset(wav, 0, sizeof wav);
    int shownRaw[ND20RX_MAX_STREAMS] = {0};
    ND20RxStats st, prev[ND20RX_MAX_STREAMS];
    memset(prev, 0, sizeof prev);

    for (int t = 1; !gQuit && (seconds <= 0 || t <= seconds); t++) {
        for (int k = 0; k < 4 && !gQuit; k++) {
            usleep(250000);
            for (int i = 0; i < nch; i++) wavPump(rx, i, &wav[i]);
        }
        printf("\n---- %d s ----\n", t);
        for (int i = 0; i < nch; i++) {
            nd20rx_get_stats(rx, i, &st, 1);
            if (st.rawCount > 0 && !shownRaw[i]) { printFirstPackets(&st); shownRaw[i] = 1; }

            /* Open the WAV once the stream format is known. */
            if (wavPrefix && !wav[i].f && st.audioSlots > 0 && st.sampleRate > 0) {
                snprintf(wav[i].path, sizeof wav[i].path, "%s_ch%d.wav", wavPrefix, st.fwChannel);
                wav[i].f = fopen(wav[i].path, "wb");
                wav[i].nch = st.audioSlots;
                wav[i].rate = (uint32_t)st.sampleRate;
                wav[i].next = -1;
                if (wav[i].f) { wavHeader(&wav[i]); printf("  recording channel %d to %s\n", st.fwChannel, wav[i].path); }
                else printf("  !! cannot create %s\n", wav[i].path);
            }

            uint64_t dp = st.packets - prev[i].packets;
            printf("  ch %-2d  %5llu pkts/s  (%llu audio, %llu empty, %llu bad)  lost: %llu\n",
                   st.fwChannel, (unsigned long long)dp,
                   (unsigned long long)(st.dataPackets - prev[i].dataPackets),
                   (unsigned long long)(st.emptyPackets - prev[i].emptyPackets),
                   (unsigned long long)(st.badPackets - prev[i].badPackets + st.notAM824 - prev[i].notAM824),
                   (unsigned long long)(st.dbcErrors - prev[i].dbcErrors));
            if (st.audioSlots > 0)
                printf("         %.0f Hz, %d audio channels, DBS %u, %u-%u blocks/packet%s\n",
                       st.sampleRate, st.audioSlots, st.dbs, st.minBlocks, st.maxBlocks,
                       st.aligned ? (st.offset == 0 ? ", clock master" : "") : "");
            for (int c = 0; c < st.audioSlots; c++) {
                char m[64];
                meter(st.peak[c], m, sizeof m);
                printf("         in %2d (routing %d ch %d)  %s\n", st.fwChannel * 8 + c + 1, st.fwChannel, c + 1, m);
            }
            prev[i] = st;
        }
        fflush(stdout);
    }

    printf("\n==== Summary ====\n");
    double refF, refH, tpf, rate;
    if (nd20rx_clock(rx, &refF, &refH, &tpf, &rate))
        printf("  Clock: locked to channel %d. Nominal %.0f Hz.\n",
               nd20rx_stream_channel(rx, nd20rx_master_stream(rx)), rate);
    else
        printf("  Clock: NOT locked (no usable audio packets).\n");
    for (int i = 0; i < nch; i++) {
        nd20rx_get_stats(rx, i, &st, 0);
        printDetail(&st);
        if (st.aligned && st.offset != 0)
            printf("    offset to clock master  : %lld frames (%s)\n", (long long)st.offset,
                   st.alignedBySYT ? "matched by SYT" : "estimated from arrival time");
        printf("    totals                  : %llu packets, %llu with audio, %llu frames, %llu lost-packet events\n",
               (unsigned long long)st.packets, (unsigned long long)st.dataPackets,
               (unsigned long long)st.framesWritten, (unsigned long long)st.dbcErrors);
    }

    for (int i = 0; i < nch; i++) wavPump(rx, i, &wav[i]);
    nd20rx_stop(rx);
    for (int i = 0; i < nch; i++)
        if (wav[i].f) {
            wavHeader(&wav[i]);
            fclose(wav[i].f);
            printf("\n  Saved %s (%llu frames, %.1f s)", wav[i].path, (unsigned long long)wav[i].frames,
                   wav[i].rate ? (double)wav[i].frames / wav[i].rate : 0.0);
        }
    printf("\nDone.\n");
    return 0;
}
