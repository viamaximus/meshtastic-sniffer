/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 CEMAXECUTER LLC
 *
 * meshtastic-sniffer: PCAP file/pipe output for Wireshark workflows.
 *
 * libpcap classic format:
 *
 *   Global header (24 bytes, written once at file open):
 *     uint32 magic         0xa1b2c3d4 (little-endian timestamps in usec)
 *     uint16 ver_major     2
 *     uint16 ver_minor     4
 *     int32  thiszone      0  (UTC)
 *     uint32 sigfigs       0
 *     uint32 snaplen       65535
 *     uint32 network       147  (DLT_USER0; safest non-misleading link type)
 *
 *   Per-packet record (16-byte header + payload):
 *     uint32 ts_sec        wall-clock seconds
 *     uint32 ts_usec       microseconds within ts_sec
 *     uint32 incl_len      bytes captured (we never truncate)
 *     uint32 orig_len      original wire length (same as incl_len)
 *     [bytes]              the frame
 *
 * FIFO mode: opens the path with O_WRONLY blocking. If no reader is
 * attached when init runs, open() blocks until one shows up. That's
 * the right behavior for `wireshark -k -i fifo` flows where the user
 * starts wireshark first; for the reverse, set up the fifo manually
 * and start wireshark before launching the sniffer.
 */

#include "pcap_out.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PCAP_MAGIC      0xa1b2c3d4u
#define PCAP_VER_MAJOR  2
#define PCAP_VER_MINOR  4
#define PCAP_SNAPLEN    65535u
#define DLT_USER0       147u

static FILE           *g_fp = NULL;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int             g_started = 0;

static void write_global_header(FILE *fp)
{
    /* Native endianness; pcap readers detect via the magic value
     * (0xa1b2c3d4 vs 0xd4c3b2a1). All-zero thiszone/sigfigs. */
    struct {
        uint32_t magic;
        uint16_t ver_major, ver_minor;
        int32_t  thiszone;
        uint32_t sigfigs;
        uint32_t snaplen;
        uint32_t network;
    } __attribute__((packed)) gh = {
        PCAP_MAGIC, PCAP_VER_MAJOR, PCAP_VER_MINOR,
        0, 0, PCAP_SNAPLEN, DLT_USER0,
    };
    fwrite(&gh, sizeof(gh), 1, fp);
    fflush(fp);
}

int pcap_out_init(const char *path, bool is_fifo)
{
    if (!path) return -1;

    if (is_fifo) {
        /* Create the FIFO if absent. EEXIST is fine -- someone may have
         * already mkfifo'd it. Other errors abort. */
        if (mkfifo(path, 0644) < 0 && errno != EEXIST) {
            fprintf(stderr, "pcap-out: mkfifo(%s): %s\n", path, strerror(errno));
            return -1;
        }
        fprintf(stderr, "pcap-out: FIFO %s -- waiting for a reader (e.g. "
                        "`wireshark -k -i %s`)...\n", path, path);
    }

    /* Plain fopen("wb") works for both regular files and FIFOs. For a
     * FIFO with no reader attached, this blocks here -- the message
     * above warns the user. */
    g_fp = fopen(path, "wb");
    if (!g_fp) {
        fprintf(stderr, "pcap-out: open(%s): %s\n", path, strerror(errno));
        return -1;
    }
    write_global_header(g_fp);
    g_started = 1;
    fprintf(stderr, "pcap-out: writing %s (DLT_USER0)\n", path);
    return 0;
}

void pcap_out_write_frame(const uint8_t *bytes, size_t len,
                          uint32_t ts_sec, uint32_t ts_usec)
{
    if (!g_started || !bytes || len == 0) return;
    if (len > PCAP_SNAPLEN) len = PCAP_SNAPLEN;

    struct {
        uint32_t ts_sec;
        uint32_t ts_usec;
        uint32_t incl_len;
        uint32_t orig_len;
    } __attribute__((packed)) ph = {
        ts_sec, ts_usec, (uint32_t)len, (uint32_t)len,
    };

    pthread_mutex_lock(&g_mu);
    if (g_fp) {
        if (fwrite(&ph, sizeof(ph), 1, g_fp) != 1 ||
            fwrite(bytes, len, 1, g_fp)      != 1) {
            /* Reader vanished (closed FIFO, full disk). Stop writing
             * cleanly so we don't spam errors on every subsequent frame. */
            fprintf(stderr, "pcap-out: write failed (%s); disabling output\n",
                    strerror(errno));
            fclose(g_fp);
            g_fp = NULL;
            g_started = 0;
        } else {
            fflush(g_fp); /* keep `wireshark -k` responsive in live mode */
        }
    }
    pthread_mutex_unlock(&g_mu);
}

void pcap_out_shutdown(void)
{
    pthread_mutex_lock(&g_mu);
    if (g_fp) { fclose(g_fp); g_fp = NULL; }
    g_started = 0;
    pthread_mutex_unlock(&g_mu);
}
