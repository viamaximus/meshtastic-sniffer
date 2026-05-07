#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 CEMAXECUTER LLC
#
# multi_subscribe.py -- reference multi-sensor aggregator.
#
# Subscribes to N meshtastic-sniffer ZMQ PUB sockets (each station has
# its own --zmq=tcp://*:7008 listening), groups same-packet observations
# by (from, packet_id) within a time window, and prints one consolidated
# line per real transmission showing which stations heard it.
#
# Validates the wire shape of the eventual fusion-dashboard pipeline
# without committing to a C binary yet. This is a development tool, not
# a production aggregator.
#
# Usage:
#   multi_subscribe.py tcp://10.0.0.5:7008 tcp://10.0.0.6:7008
#   multi_subscribe.py --window 3 tcp://localhost:7008 tcp://localhost:7009
#
# Requires: pyzmq  (pip install pyzmq)

import argparse
import json
import sys
import time
from collections import defaultdict

try:
    import zmq
except ImportError:
    print("ERROR: pyzmq not installed. Run: pip install pyzmq", file=sys.stderr)
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("endpoints", nargs="+",
                    help="ZMQ endpoints to subscribe to (e.g. tcp://10.0.0.5:7008)")
    ap.add_argument("--window", type=float, default=5.0,
                    help="Dedup window in seconds (default 5)")
    ap.add_argument("--max-frames", type=int, default=0,
                    help="Stop after N consolidated frames (0 = unlimited)")
    args = ap.parse_args()

    ctx = zmq.Context.instance()
    poller = zmq.Poller()
    socks = []
    for ep in args.endpoints:
        s = ctx.socket(zmq.SUB)
        s.setsockopt(zmq.SUBSCRIBE, b"")
        s.connect(ep)
        poller.register(s, zmq.POLLIN)
        socks.append(s)
        print(f"# subscribed to {ep}", file=sys.stderr)

    # cluster_key -> {'first_seen': t, 'observations': [(station, snr_db, rssi_db, lat, lon)]}
    clusters: dict = {}
    consolidated = 0

    try:
        while True:
            events = dict(poller.poll(timeout=int(args.window * 1000 / 4)))
            now = time.time()

            for s in events:
                raw = s.recv()
                try:
                    p = json.loads(raw)
                except Exception:
                    continue
                fr = p.get("from")
                pid = p.get("packet_id")
                if fr is None or pid is None:
                    # STATS, OFF_GRID_LORA, REPLAY_SUSPECTED -- not packet data
                    continue
                station = p.get("station") or "(unnamed)"
                obs = (
                    station,
                    p.get("snr_db"),
                    p.get("rssi_db"),
                    p.get("station_lat"),
                    p.get("station_lon"),
                )
                key = (fr, pid)
                if key not in clusters:
                    clusters[key] = {"first_seen": now, "observations": [], "frame": p}
                clusters[key]["observations"].append(obs)

            # Flush clusters whose first-seen is older than the window.
            ready = [k for k, v in clusters.items()
                     if now - v["first_seen"] > args.window]
            for key in ready:
                v = clusters.pop(key)
                fr, pid = key
                stations = sorted(set(o[0] for o in v["observations"]))
                snrs = [f"{o[0]}={o[1]:.1f}dB" for o in v["observations"]
                        if o[1] is not None]
                preset = v["frame"].get("preset", "?")
                ch_name = v["frame"].get("channel_name") or "(encrypted)"
                hops = "{}/{}".format(v["frame"].get("hop_limit", "?"),
                                     v["frame"].get("hop_start", "?"))
                print(f"{fr:>11} pid={pid:>10} {preset:<11} ch={ch_name:<12} "
                      f"hop={hops}  heard-by={len(stations)}/{len(socks)} "
                      f"[{', '.join(snrs)}]")
                consolidated += 1
                if args.max_frames and consolidated >= args.max_frames:
                    return
    except KeyboardInterrupt:
        print("# interrupted", file=sys.stderr)
    finally:
        for s in socks:
            s.close()
        ctx.term()


if __name__ == "__main__":
    main()
