#!/usr/bin/env python3
"""
gr-lora_sdr TX stage dump.

Runs the gr-lora_sdr TX block chain end-to-end on a fixed input message
("ABC" by default), capturing the byte/symbol stream between every block.
Each tap is written to a fixture file under `tests/fixtures/lora_stages/`
so our C decode-side ports can unit-test against ground truth.

Stages tapped (in order):
  0. input bytes                           ->  00_input.bin
  1. whitening output                      ->  01_whitening.bin
  2. header output (with length+cr+crc)    ->  02_header.bin
  3. add_crc output                        ->  03_add_crc.bin
  4. hamming_enc output (codewords)        ->  04_hamming.bin
  5. interleaver output (interleaved syms) ->  05_interleaver.bin
  6. gray_demap output (TX-side gray-map)  ->  06_gray.bin
  7. modulate output (cf32 IQ samples)     ->  07_modulate.cf32

Plus a JSON fixture descriptor with sf/cr/bw/samp_rate so the C-side
tests know how to interpret each file.

Copyright (c) 2026 CEMAXECUTER LLC
SPDX-License-Identifier: GPL-3.0-or-later
"""

import argparse
import json
import os
import sys
import time

from gnuradio import gr, blocks, lora_sdr


def make_tag(key: str, value, offset: int = 0):
    t = gr.tag_t()
    t.offset = offset
    t.key = gr.pmt.intern(key)
    t.value = gr.pmt.from_long(value) if isinstance(value, int) else gr.pmt.intern(value)
    return t


class StageDumpTX(gr.top_block):
    """gr-lora_sdr TX with a file_sink after every block."""

    def __init__(self, payload: bytes, sf: int, cr: int, bw: int, samp_rate: int,
                 outdir: str, preamble_len: int = 16):
        gr.top_block.__init__(self, "lora-stage-dump")
        n = len(payload)

        # Source: the raw input bytes with a packet_len tag.
        self.src = blocks.vector_source_b(list(payload), False, 1,
                                          [make_tag("packet_len", n)])

        # Build the TX chain identical to lora_TX.py.
        gr_cr = max(1, min(4, cr - 4)) if cr >= 5 else cr  # accept either convention
        self.whitening = lora_sdr.whitening(False, True, ',', 'packet_len')
        self.header = lora_sdr.header(False, True, gr_cr)
        self.add_crc = lora_sdr.add_crc(True)
        self.hamming = lora_sdr.hamming_enc(gr_cr, sf)
        self.interleaver = lora_sdr.interleaver(gr_cr, sf, 0, bw)
        self.gray = lora_sdr.gray_demap(sf)
        inter_pad = int(samp_rate / bw * 32)
        self.modulate = lora_sdr.modulate(sf, samp_rate, bw, [0x12], inter_pad, preamble_len)

        # File sinks at each tap. Note: header / hamming_enc / interleaver /
        # gray_demap take/produce uint8_t streams, not nibbles -- whatever the
        # native item size is for that block port.
        self.tap_input    = blocks.file_sink(gr.sizeof_char, f"{outdir}/00_input.bin", False)
        self.tap_whiten   = blocks.file_sink(gr.sizeof_char, f"{outdir}/01_whitening.bin", False)
        self.tap_header   = blocks.file_sink(gr.sizeof_char, f"{outdir}/02_header.bin", False)
        self.tap_crc      = blocks.file_sink(gr.sizeof_char, f"{outdir}/03_add_crc.bin", False)
        self.tap_hamming  = blocks.file_sink(gr.sizeof_char, f"{outdir}/04_hamming.bin", False)
        self.tap_inter    = blocks.file_sink(gr.sizeof_int, f"{outdir}/05_interleaver.bin", False)
        self.tap_gray     = blocks.file_sink(gr.sizeof_int, f"{outdir}/06_gray.bin", False)
        self.tap_iq       = blocks.file_sink(gr.sizeof_gr_complex, f"{outdir}/07_modulate.cf32", False)
        for s in (self.tap_input, self.tap_whiten, self.tap_header, self.tap_crc,
                  self.tap_hamming, self.tap_inter, self.tap_gray, self.tap_iq):
            s.set_unbuffered(False)

        # Connections: data path + tee at each stage.
        self.connect(self.src, self.tap_input)
        self.connect(self.src, self.whitening)
        self.connect(self.whitening, self.tap_whiten)
        self.connect(self.whitening, self.header)
        self.connect(self.header, self.tap_header)
        self.connect(self.header, self.add_crc)
        self.connect(self.add_crc, self.tap_crc)
        self.connect(self.add_crc, self.hamming)
        self.connect(self.hamming, self.tap_hamming)
        self.connect(self.hamming, self.interleaver)
        self.connect(self.interleaver, self.tap_inter)
        self.connect(self.interleaver, self.gray)
        self.connect(self.gray, self.tap_gray)
        self.connect(self.gray, self.modulate)
        self.connect(self.modulate, self.tap_iq)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", default="tests/fixtures/lora_stages")
    ap.add_argument("--text", default="ABC")
    ap.add_argument("--sf",  type=int, default=11)
    ap.add_argument("--cr",  type=int, default=5,
                    help="our convention: 5/6/7/8 for 4/5..4/8 (or 1..4 in gr-lora_sdr terms)")
    ap.add_argument("--bw",  type=int, default=250000)
    ap.add_argument("--samp-rate", type=int, default=250000)
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)

    payload = args.text.encode("utf-8")
    print(f"input: {payload!r} ({len(payload)} bytes)")
    print(f"running gr-lora_sdr TX -> {args.outdir}/")

    tb = StageDumpTX(payload, args.sf, args.cr, args.bw, args.samp_rate, args.outdir)
    tb.start()
    time.sleep(0.5)
    tb.stop(); tb.wait()

    # Descriptor.
    desc = {
        "input":     "00_input.bin",
        "whitening": "01_whitening.bin",
        "header":    "02_header.bin",
        "add_crc":   "03_add_crc.bin",
        "hamming":   "04_hamming.bin",
        "interleaver": "05_interleaver.bin",
        "gray":      "06_gray.bin",
        "modulate":  "07_modulate.cf32",
        "params": {
            "sf": args.sf,
            "cr": args.cr,
            "bw": args.bw,
            "samp_rate": args.samp_rate,
            "text": args.text,
        },
    }
    with open(os.path.join(args.outdir, "fixture.json"), "w") as f:
        json.dump(desc, f, indent=2)

    print("\nTap sizes:")
    for f in sorted(os.listdir(args.outdir)):
        sz = os.path.getsize(os.path.join(args.outdir, f))
        print(f"  {f:24s}  {sz} bytes")


if __name__ == "__main__":
    main()
