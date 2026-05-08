/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 CEMAXECUTER LLC
 *
 * meshtastic-sniffer: PCAP file/pipe output for Wireshark workflows.
 *
 * Writes received LoRa frames in libpcap format using DLT_USER0 (147)
 * so analysts can `wireshark -k -i fifo` against a live sniffer or
 * crack a saved capture open offline. Each frame is written with the
 * 16-byte radio header followed by the (still-encrypted-on-the-wire)
 * payload bytes -- the same bytes that hit the LoRa demod.
 *
 * Two output modes:
 *   - file: ordinary regular file. Optionally rotates at a size cap
 *     (see pcap_out_init parameters).
 *   - fifo: named pipe (mkfifo'd if not already present). Use this with
 *     'wireshark -k -i /tmp/meshtastic.fifo' for live capture.
 */

#ifndef PCAP_OUT_H
#define PCAP_OUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Initialise pcap output. `path` is a regular-file or FIFO path;
 * `is_fifo` true means mkfifo if absent and treat reader-blocking
 * gracefully. Returns 0 on success, -1 on failure. */
int pcap_out_init(const char *path, bool is_fifo);

/* Write one frame. Caller-supplied wall-clock seconds + microseconds
 * for the pcap timestamp; the frame bytes should be the on-the-wire
 * 16-byte radio header + ciphertext (i.e. exactly what hit the LoRa
 * demod). Thread-safe; one mutex serializes writes. */
void pcap_out_write_frame(const uint8_t *bytes, size_t len,
                          uint32_t ts_sec, uint32_t ts_usec);

void pcap_out_shutdown(void);

#endif /* PCAP_OUT_H */
