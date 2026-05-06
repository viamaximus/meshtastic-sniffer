# meshtastic-fusion

> **Status: work in progress.** Functional skeleton, not battle-tested.

Multi-station aggregator for [meshtastic-sniffer](../README.md). Subscribes to N sniffer ZMQ feeds, presents one dashboard over all of them, and fans command operations (add key, promote off-grid, etc.) to every registered sensor in one click.

A separate Go binary so it can be deployed on a different host than the SDR-attached sniffers. The sniffer side stays C; fusion is HTTP/SSE/state-shuffling work that's a better fit for Go's stdlib.

## What it does today

- **Subscribes** to one or more sniffers' ZMQ PUB sockets (`--zmq=tcp://*:7008` on the sniffer side)
- **Persistent sensor registry** at `~/.config/meshtastic-fusion/sensors.json` (or `--sensors-file=PATH`); add/remove via the dashboard's Sensors tab or `POST /api/sensors`, `DELETE /api/sensors/<name>`
- **Live dashboard** at `--listen=:9000` with five tabs:
  - **Live**: Leaflet map with per-station markers (distinct color per sensor) and per-node markers, plus side tables showing stations and recently-seen nodes with "heard by" attribution
  - **Activity**: per-preset cards aggregated across all sensors, with each card showing per-sensor breakdown (frame counts, decrypt%)
  - **Topology**: force-directed graph; each station is a pinned colored node, each transmitting node has dashed pseudo-edges to every station that's heard it (SNR-colored)
  - **Sensors**: registry table with live health (green/yellow/red), frames seen, decrypt%, msps, last event, plus an add form
  - **Config**: command fan-out (Add key, Channel-share URL, Add extra freq, CoT multicast) — each form POSTs to every sensor's `/api/*` and reports per-sensor status
- **C2 fan-out** via HTTP `POST /api/fanout/<endpoint>` — fans to every registered sensor's matching `/api/<endpoint>` in parallel, with optional bearer token from the sensor's registry entry
- **SSE replay ring** so a browser refresh restores dashboard state from the last 1024 events
- **TX consolidation events** when the same packet is heard by multiple sensors within a short window (`--window=3s` default), emitted as `{"event":"TX", ...}` to SSE subscribers

## Quick start

Build once:

```bash
cd fusion
go build -o meshtastic-fusion ./...
```

Run a single sniffer + fusion on the same machine:

```bash
# Terminal 1: sniffer with ZMQ telemetry exposed
./build/meshtastic-sniffer --hackrf --rate=20000000 --center=915000000 \
    --presets=all --region=US --keys=default \
    --web=8888 --zmq=tcp://*:7008 --station-id=hackrf-rx

# Terminal 2: fusion
./fusion/meshtastic-fusion \
    --listen=:9000 \
    --sensors-file=$HOME/.config/meshtastic-fusion/sensors.json
```

Then browser to `http://localhost:9000/`, Sensors tab, **Add**:

- name: `hackrf-rx`
- zmq: `tcp://127.0.0.1:7008`
- api: `http://127.0.0.1:8888`

Two-station deployment? Same pattern, different ports / different machines:

```bash
# rooftop sensor
./build/meshtastic-sniffer ... --zmq=tcp://*:7008 --station-id=rooftop --gpsd

# basement sensor
./build/meshtastic-sniffer ... --zmq=tcp://*:7008 --station-id=basement --gpsd

# fusion (could be on a third machine)
./fusion/meshtastic-fusion --listen=:9000 \
    --sensors-file=$HOME/.config/meshtastic-fusion/sensors.json
```

Add both sensors via the dashboard. Multi-station benefits unlock once 2+ are connected: which sensor heard which packet at what SNR, "heard by N/M" badges, cross-sensor `(from, packet_id)` consolidation in the TX feed.

## Honest limitations (work in progress)

- **No persistence beyond sensor registry** -- nodes / activity / topology state lives in-browser; if the fusion process restarts, the SSE replay ring is empty until traffic flows again. A long-term archive for fusion-side aggregated data isn't built yet (the per-sensor `--archive=DIR` exists on the sniffer side).
- **STATS heartbeat events from sniffers don't reach fusion today** -- the sniffer publishes STATS only over its local `/events` SSE, not over ZMQ. Per-sensor msps in the Sensors tab will show `--`. To be fixed by routing STATS through the regular feed.
- **CurveZMQ -- sniffer-side only.** The sniffer accepts `--zmq-curve-keygen=PATH` (writes a Z85 keypair) and `--zmq-curve-secret=PATH` (enables CURVE on its PUB socket). Fusion's Go subscriber doesn't yet authenticate to a CURVE-protected PUB because `go-zeromq/zmq4` v0.17 declares the security type but doesn't implement the handshake. For non-VPN deployments needing wire encryption today, a libzmq-based proxy in front of fusion still works; the sniffer-side option pair is shipped so an operator's keys are stable across the upgrade.
- **DEALER C2 path -- shipped, but UI is read-only.** `--c2-dealer=tcp://fusion:7009` on the sniffer opens an outbound DEALER socket; the fusion's `--c2-router=:7009` accepts the connection, tracks heartbeats, and the fanout layer prefers DEALER over HTTP for any sensor with a live session. Sensors tab shows a `DEALER`/`HTTP` badge per sensor. Heartbeat-status histograms and per-command latency aren't surfaced yet.
- **No TDOA / multilateration** -- this is the multi-station-aggregation tier (Phase 2.5). Time-disciplined GPSDO captures + a hyperbolic multilateration solver are a separate project (Phase 3 in the roadmap), not part of fusion's current scope.
- **No authentication on fusion's own dashboard** -- anyone who can reach the listen port can manage the sensor registry and fan out commands. Operate behind a VPN / authenticating reverse proxy.
- **Ports / fields are inherited from the sniffer** -- if the sniffer's event JSON shape changes (new fields), fusion's dashboard JS may need parallel updates.

Honest take: fusion is a *path* toward a polished multi-sensor product, not the destination. It validates the architecture and gives operators something usable. A more robust fusion-style server (proper auth, persistent DB, queue-based ingest, deployable as a service) is a follow-on project.

## License

GPL-3.0-or-later. Same license + author as the parent project. See [../LICENSE](../LICENSE).
