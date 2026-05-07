/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 CEMAXECUTER LLC
 *
 * Stubs for symbols the recover binary doesn't use, but that the
 * shared mesh_packet.c references. Defining them here avoids dragging
 * options.c (CLI parser, irrelevant to a cracker) and psk_dict.c (the
 * live-radio dictionary attack, replaced here by the offline pcap
 * driver in meshtastic-recover.c) into the link.
 */

#include <stdint.h>
#include <stddef.h>

/* options.c global: log level (0 = silent). Cracker is silent. */
int verbose = 0;

/* psk_dict.c hook fired by mesh_packet_decode on undecrypted frames in
 * the sniffer's live-radio path. The cracker drives the same loop from
 * disk and skips this side path. */
void psk_dict_enqueue(const uint8_t *frame, size_t frame_len,
                      float rssi_db, float snr_db,
                      int sf, int cr, int bw_hz)
{
    (void)frame; (void)frame_len; (void)rssi_db; (void)snr_db;
    (void)sf; (void)cr; (void)bw_hz;
}
