# meshtastic-sniffer

Wideband passive Meshtastic LoRa receiver written in C. Captures one wide IQ slice from a single SDR and decodes **every Meshtastic channel and preset that fits in the slice simultaneously** -- no per-channel hopping, no missed packets. With keys supplied, decrypts in parallel: text messages, GPS positions, node info, telemetry, routing, traceroute, ATAK plugin packets, and more -- all surfaced from one capture.

Sister project to [iridium-sniffer](https://github.com/alphafox02/iridium-sniffer) and [inmarsat-sniffer](https://github.com/alphafox02/inmarsat-sniffer). Same SDR backend matrix, same threading style, same output ecosystem.

## Features

- Polyphase filterbank channelizer (AVX2/SSE4.2/NEON SIMD), one wide IQ stream into N parallel per-channel basebands at `Fs/M` each
- All Meshtastic regions: US (902-928), EU_868, EU_433, CN, JP, ANZ, KR, TW, RU, IN, NZ_865, TH, UA_433, UA_868, MY_433, MY_919, SG_923, KZ_433, KZ_863, NP_865, BR_902, PH_433/868/915, LORA_24
- All 9 standard presets: ShortTurbo, ShortFast, ShortSlow, MediumFast, MediumSlow, LongFast, LongMod, LongSlow, LongTurbo
- Multi-key AES-128 / AES-256-CTR with 1-byte channel-hash routing -- adding more keys does NOT slow per-packet decode (steady state: 1 AES op per packet)
- Per-port protobuf decode for `TEXT_MESSAGE_APP`, `POSITION_APP`, `NODEINFO_APP`, `TELEMETRY_APP` (DeviceMetrics + EnvironmentMetrics + PowerMetrics), `ROUTING_APP`, `TRACEROUTE_APP`, `WAYPOINT_APP`, `ADMIN_APP`, `NEIGHBORINFO_APP`, `KEY_VERIFICATION_APP`, `MAP_REPORT_APP`, `ATAK_PLUGIN`, `REMOTE_HARDWARE_APP`, `DETECTION_SENSOR_APP`, `STORE_FORWARD_APP`, `PAXCOUNTER_APP`. Other ports surface as raw bytes in JSON.
- ATAK port 72 decoder for `TAKPacket`: callsign, team, role, battery, PLI (lat/lon/alt/speed/course), GeoChat. PLIs republished as CoT XML over multicast (see Outputs).
- Off-grid LoRa scanner: occupied-bandwidth estimator + 3-confirm threshold flags LoRa-shaped energy outside the configured channel grid
- Per-frame RSSI/SNR + on-failure CRC + drift-only CFO telemetry carried through into the JSON event and the dashboard
- Built-in web dashboard (Live map / Activity / Topology / Config), runtime key + extra-freq additions without restart
- JSON, UDP, MQTT, ZMQ PUB, CoT XML multicast, daily-rotated gzipped JSONL archive, libpcap streaming export output sinks
- PSK dictionary attack against undecrypted frames (`--psk-wordlist=PATH`), GPSDO-friendly geofence ENTRY/EXIT alerts (`--geofence=PATH`), CurveZMQ on the PUB socket (`--zmq-curve-secret=PATH`)
- Replay-attack flagging on duplicate `(from, packet_id)` tuples beyond the normal mesh retransmit window
- Multi-station deployment: emit ZMQ telemetry to a [meshtastic-fusion](fusion/) aggregator, optionally self-register via `--announce-to=URL`, optional outbound DEALER socket (`--c2-dealer=tcp://fusion:7009`) for NAT-friendly C2
- `--schema` dumps the JSON event schema (JSON Schema 2020-12) so SIEM consumers can validate without guessing
- AddressSanitizer- and ThreadSanitizer-clean build / smoke-test pipeline

## Use cases

The same passive observer fits three operational shapes:

- **Operator / surveyor** -- ham, EmComm coordinator, community organizer. "What Meshtastic networks operate in my area? How busy is each preset? Are nodes reaching each other?" Live map + Activity + Topology tabs. Default keys supplied.
- **Pen tester / red team** -- authorized engagement on a target's mesh. "Do they use default channel keys? Are admin commands flying around unsigned? What's their actual range envelope?" JSON feed for tooling, libpcap streaming export (`--pcap=PATH` / `--pcap-fifo=PATH` for live Wireshark), PSK dictionary attack against unknown channels (`--psk-wordlist=PATH`).
- **Defensive monitoring** -- site security wanting to know what's leaving the perimeter via LoRa. Off-grid scanner + daily-rotated gzipped JSONL archive (`--archive=DIR`) + geofence ENTRY/EXIT alerts (`--geofence=PATH`).

The core is honest passive observation; the use case is mostly which outputs you wire up and which dashboard tabs you stare at.

## Hardware capacity

The number of channels you stare at simultaneously is set by the SDR's analog bandwidth and your `--rate`. **Any rate works** -- the channelizer auto-fits whichever channels of the configured grid land inside `[center - rate/2, center + rate/2]`. 56 MHz is the threshold to capture *every* US-band 250 kHz slot from one stare; below that you cover a contiguous subset that the binary picks for you.

| SDR | Bandwidth | Coverage at default rate | Notes |
|-----|-----------|--------------------------|-------|
| HackRF One | 20 MHz | 41/104 US LongFast slots, all wider-spaced presets | Most common config; partial cover but every preset is visible |
| BladeRF 2.0 | 56 MHz | All 104 US LongFast slots | AD9361, full ISM band coverage |
| USRP B210 | 56 MHz | All 104 US LongFast slots | UHD-driven |
| SDRplay (RSPdx, RSP1A) | 10 MHz | One BW group + adjacent presets | Native API |
| Airspy R2 / Mini | 10 MHz | One BW group + adjacent presets | |
| RTL-SDR (R820T) | 2.4 MHz | One BW group, ~9 LongFast slots | Cheap entry point |
| Custom via SoapySDR | varies | -- | Generic |
| VITA-49 / VRT (network) | varies | -- | Remote IQ over UDP from any sender that emits VITA-49 (`[BIND:]PORT`, big-endian VRT signal-data, optional VRL wrapper). Same wire format as iridium-sniffer / inmarsat-sniffer; if a sender works for those, it works for this. |
| IQ file replay | -- | -- | Offline; auto-loads `.sigmf-meta` sibling for rate/freq/format |

`--list` enumerates all attached SDRs across every compiled-in backend.

`./meshtastic-sniffer --rate=20000000 --region=US --presets=all` -- without `--center`, the binary picks a sensible center from region + preset midpoints, logs the resolved coverage window, and warns if a user-supplied `--center` falls outside the configured region.

## Outputs

- **JSON feed** to stdout (always when running) and to UDP endpoints (`--feed=HOST:PORT`, repeatable)
- **MQTT** publish (`--mqtt=HOST[:PORT]`, topic `meshtastic/<station-id>` by default, override with `--mqtt-topic`)
- **ZMQ PUB** for multi-consumer (`--zmq=tcp://*:7008`); optional CurveZMQ encryption with `--zmq-curve-secret=PATH` (generate keypair via `--zmq-curve-keygen=PATH`)
- **CoT XML multicast** (`--cot-multicast=239.2.3.1:6969`) -- republishes every positioned node (regular Meshtastic POSITION packets *and* ATAK PLIs) as Cursor-on-Target XML to a multicast group. Any LAN ATAK-CIV / WinTAK / iTAK picks them up automatically -- no TAK Server required. CoT UIDs are prefixed with `--station-id` when set so multi-station deployments don't collide.
- **PCAP** streaming export (`--pcap=PATH` for a rotating file, `--pcap-fifo=PATH` for a named pipe Wireshark can attach to live). Each LoRa frame is wrapped in DLT_USER0 with a libpcap header.
- **Daily-rotated gzipped JSONL archive** (`--archive=DIR`) -- every emitted event appended to `DIR/meshtastic-YYYYMMDD.jsonl.gz`, rotated at UTC midnight. SIEM-friendly.
- **Built-in web dashboard** (`--web=8888`) -- four tabs (see below)

## Web dashboard

`--web=8888` opens four tabs:

- **Live** -- Leaflet map with node markers + trail polylines (last 8 fixes per node). Nodes table with search box, sortable columns (ID / Name / SNR / Frames / Last seen), CSV export. Click any row to slide in a per-node drawer: name + id, metrics, last-60 SNR sparkline, recent text messages, recent positions, channels-seen-on. Channels table (by hash). Messages and Discoveries panels.
- **Activity** -- per-preset cards, one per Meshtastic preset (ShortTurbo, ShortFast, ShortSlow, MediumFast, MediumSlow, LongFast, LongMod, LongSlow, LongTurbo). Each card shows fpm (60-second rolling), cumulative frames, decrypted/total channel ratio, sparkline, and the channel sub-list (channel name when decrypted, `(encrypted)` otherwise). Idle presets render greyed.
- **Topology** -- force-directed graph. Nodes sized by frame count, edges colored by SNR. Real edges from `NEIGHBORINFO_APP` packets and resolved relay-hop hints. A synthetic "RX" station at canvas center has a faint dashed pseudo-edge to every node this sniffer hears, so even a sparse mesh with no NEIGHBORINFO traffic gives a useful picture (what *you* are receiving). Hover to highlight; click any real node to open the drawer.
- **Config** -- runtime forms for adding keys, importing `meshtastic.org/e/` channel-share URLs, adding extra-frequency decoder slots, changing CoT multicast destination -- all without restarting the binary.

Equivalent endpoints exposed at `POST /api/keys`, `POST /api/share-url`, `POST /api/extra-freq`, `POST /api/cot-multicast` for scripting. Optional bearer-token auth via `--api-token=SECRET` (clients send `Authorization: Bearer SECRET`).

## JSON event format

`./meshtastic-sniffer --schema` dumps the canonical JSON Schema 2020-12 for every event the binary emits. Key per-frame fields: `from`, `to`, `packet_id`, `channel_hash` (1-byte routing hash from the radio header), optional `slot_id` (which polyphase channelizer slot caught the frame), `hop_limit`/`hop_start`, `rssi_db`/`snr_db`. Quality telemetry only appears when actionable: `payload_crc_ok: false` if a CRC was present and failed, `cfo_hz` only when |drift| > 100 Hz. Per-port decoded fields (text, lat/lon, telemetry, etc.) appear when the channel key is known and the payload parses. Top-level `event` discriminator distinguishes `STATS`, `OFF_GRID_LORA`, `REPLAY_SUSPECTED`, `GEOFENCE_ENTRY`/`GEOFENCE_EXIT`, `PSK_DISCOVERED` from regular packet events.

The JSON wire format changed in May 2026: the per-event `channel` field was renamed to `channel_hash` to reduce confusion with both decoder slot index and human-readable channel name. Downstream consumers reading the old field name need a one-line rename.

## Stats heartbeat

Every 5 seconds the binary prints a one-line summary to stderr so you can tell at a glance whether samples are flowing and frames are being decoded:

```
[stats] 18.45 Msps in, 12 LoRa frames, 9 decrypted
```

`off-grid hits` is appended only when the scanner is enabled (`--scan*` or `--alert-off-grid`).

## Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

Dependencies: cmake >= 3.9, FFTW3 (float), pthreads, OpenSSL (AES-CTR), and at least one SDR library for live capture. CMake auto-detects every backend you have installed.

```bash
sudo apt install build-essential cmake pkg-config libfftw3-dev libssl-dev
sudo apt install librtlsdr-dev libhackrf-dev libbladerf-dev libuhd-dev \
                 libsoapysdr-dev libairspy-dev
# SDRplay native API: install from sdrplay.com/api
# Optional output sinks
sudo apt install libmosquitto-dev libzmq3-dev
```

## First-time use (30 seconds)

You have a Meshtastic node and an SDR. You want to see what your node and its neighbours are saying.

1. **Get your channel key.** In the Meshtastic phone app, open the channel, tap "Share Channel," and copy the URL -- it looks like `https://meshtastic.org/e/#CgM...`. That URL contains the channel name and the AES key in a small protobuf payload.

2. **Plug in the SDR.** Run `./meshtastic-sniffer --list` to confirm it's detected.

3. **Run with a sensible default + paste your channel URL:**

   ```bash
   ./meshtastic-sniffer --hackrf --share-url='https://meshtastic.org/e/#CgM...' --web=8888
   ```

   The binary picks a sample rate and center frequency from the SDR + region (`US` by default -- override with `--region=EU_868` etc.). It opens the dashboard at `http://localhost:8888`.

4. **If nothing shows up after a minute**, check stderr -- the binary prints loud warnings when no samples are flowing or no LoRa frames have decoded. Common causes: gain too low (`--gain=40`), wrong region, no node in range. The HackRF default is LNA=0 / VGA=30 -- right for close traffic but turn `--gain` up for distant captures.

5. **Adding more channels later:** open the dashboard, click **Config** tab, paste another `meshtastic.org/e/` URL, hit Add. Done -- no restart needed.

## Quickstart

```bash
# Casual: defaults pick up rate/center for your SDR + region.
./meshtastic-sniffer --hackrf --keys=default --web=8888

# Or paste your channel-share URL once and skip the dashboard:
./meshtastic-sniffer --hackrf --share-url='https://meshtastic.org/e/#CgM...' --web=8888

# Power: stare at every preset on US 902-928, dump per-channel stats every 5s,
# tee raw IQ to disk for later replay, multi-station-tagged feed to a collector:
./meshtastic-sniffer --bladerf --presets=all \
                    --keys-file=$HOME/.config/meshtastic-sniffer/keys \
                    --stats-json=/run/meshsniff/stats.json \
                    --iq-record=/data/capture-$(date +%s).cs8 \
                    --feed=collector:5588 --station-id=basement-rx --web=8888

# Receive over a network feed (any sender that emits VITA-49 VRT):
./meshtastic-sniffer --vita49=4991 --keys=default --web=8888
# (sample rate + center freq come from the VITA-49 IF-context packets
#  automatically when the sender emits them; otherwise pin via --rate/--center)

# US LongFast on a HackRF with explicit overrides:
./meshtastic-sniffer --hackrf --region=US --presets=LongFast \
                    --rate=20000000 --center=910000000 \
                    --keys=default --web=8888

# Replay an IQ capture (sample rate / freq / format pulled from .sigmf-meta):
./meshtastic-sniffer --file=capture.cf32 --keys=default

# Multi-output: stdout JSON + UDP feed + MQTT + ZMQ + CoT multicast + web
./meshtastic-sniffer --hackrf --keys=LongFast=default,Ops=hex:00112233...ff \
                    --feed=collector:5588 --mqtt=mqtt.local \
                    --zmq=tcp://*:7008 --cot-multicast=239.2.3.1:6969 \
                    --web=8888 --station-id=basement-rx

# Off-grid scan only (no decode, just discover non-standard LoRa freqs):
./meshtastic-sniffer --hackrf --scan --alert-off-grid

# List every SDR you have plugged in:
./meshtastic-sniffer --list

# Run the self-tests (channelizer routing + AES end-to-end + JSON output):
./meshtastic-sniffer --selftest
```

## Generating test IQ from gr-lora_sdr

If you have `gnuradio` and `gr-lora_sdr` installed (`python3 -c "from gnuradio import lora_sdr"` should succeed), you can generate a known-good Meshtastic-shaped IQ file without any radio hardware:

```bash
python3 tools/gen_meshtastic_iq.py --out=/tmp/meshtastic_test.cf32 \
                                    --text="Hello" --sf=11 --bw=250000 --cr=5
./meshtastic-sniffer --file=/tmp/meshtastic_test.cf32 --rate=250000 \
                    --center=903000000 \
                    --extra-freq=903000000:bw=250000:sf=11:cr=5 \
                    --keys=default
```

The generator builds a real Meshtastic frame (16-byte radio header + AES-128-CTR encrypted Data envelope with channel-hash for the default key) and runs it through gr-lora_sdr's modulator. Useful both as a smoke test of the whole pipeline and for iterating on the LoRa demod's sync/header/payload decoding.

`MESHTASTIC_LORA_TRACE=1` enables a per-symbol state-machine trace on stderr, useful for debugging decode against a known-good reference.

## Self-test and smoke test

`./meshtastic-sniffer --selftest` runs two checks:

1. **Channelizer**: synthesizes a 0.1 sec tone at 902.625 MHz inside a 20 MHz capture centered on 910 MHz; configures four 250 kHz channels at the US LongFast slot 0..3 grid; verifies the tone lands in slot 2 with the expected power profile.
2. **AES + multi-key + protobuf**: builds a synthetic Meshtastic packet (`TEXT_MESSAGE_APP`, payload `"Hello"`, encrypted with the default key), runs it through the decode path, and verifies the callback fires with the right port + payload + channel name.

`bash tests/test_smoke.sh` adds SigMF auto-config, `--list`, web `/api/*` round-trip, STATS SSE heartbeat, and stats heartbeat. Both tests run clean under AddressSanitizer + UndefinedBehaviorSanitizer; ThreadSanitizer reports no data races on the keyset rwlock under concurrent `/api/keys` POST load.

## Honest limitations

Things this tool does not do today:

- No direction finding from a single SDR -- amplitude alone can't localize an emitter
- 16 of the known port numbers are decoded into structured fields; others surface as raw bytes in the JSON event
- AdminMessage signature verification (ed25519) is not implemented yet -- admin packets are decoded but not validated
- CurveZMQ is sniffer-side only; the Go-based [meshtastic-fusion](fusion/) aggregator can't yet authenticate to a CURVE-protected PUB (limitation of `go-zeromq/zmq4` v0.17). Use a libzmq-based proxy or VPN-gate the link.

## License

GPL-3.0-or-later. See `LICENSE`. Copyright (c) 2026 CEMAXECUTER LLC.

This project is independent of and not affiliated with Meshtastic. "Meshtastic" is a trademark of [Meshtastic LLC](https://meshtastic.org). Protocol constants used here (default PSK, channel hash, AES-CTR nonce layout, region/preset tables) are interoperability facts derived from the upstream firmware at <https://github.com/meshtastic/firmware> (also GPL-3.0-or-later); no proprietary code is included.

### Upstream attribution

- **gr-lora_sdr** by Joachim Tapparel @ EPFL TCL Lab (<https://github.com/tapparelj/gr-lora_sdr>, GPL-3.0-or-later) -- significant portions of `lora.c`'s bit-level decode path (hard-decode Hamming, deinterleave, gray, dewhiten, preamble-mode-vote) are ported from gr-lora_sdr and verified bit-exact against its stage outputs. Per-stage citations appear inline at the relevant call sites in `lora.c`.
- **Meshtastic firmware** (<https://github.com/meshtastic/firmware>, GPL-3.0-or-later) -- the wire format, default PSK, simpleN PSK derivation, channel-hash function, AES-CTR nonce layout, region/preset tables, and port number assignments are all derived from the upstream firmware. Implementation here is original; only the on-the-air constants come from upstream.
- **Felipe Kersting** -- `blocking_queue.h` and `fair_lock.h` are vendored MIT-licensed primitives (Copyright (c) 2020). License preserved in each file.
