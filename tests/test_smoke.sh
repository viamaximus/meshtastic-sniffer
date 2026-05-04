#!/bin/bash
# meshtastic-sniffer smoke test.
#
# Runs the binary's --selftest, then spawns it against a synthetic IQ
# noise file and exercises every /api/* endpoint via curl.  Returns 0
# on full pass, non-zero on any failure.
#
# Copyright (c) 2026 CEMAXECUTER LLC
# SPDX-License-Identifier: GPL-3.0-or-later

set -e
PORT=${PORT:-8911}

# Find the binary: explicit $BIN env var, then ./meshtastic-sniffer (project
# root after a make install-style copy), then build/meshtastic-sniffer (the
# default cmake out-of-tree build path). Lets the test run from either the
# project root or the build directory without symlink dances.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if [[ -n "$BIN" && -x "$BIN" ]]; then :;
elif [[ -x "$HERE/meshtastic-sniffer" ]]; then BIN="$HERE/meshtastic-sniffer";
elif [[ -x "$HERE/build/meshtastic-sniffer" ]]; then BIN="$HERE/build/meshtastic-sniffer";
else
    echo "FAIL: meshtastic-sniffer not found. Tried \$BIN, $HERE/, $HERE/build/."
    exit 1
fi

echo "== Test 1: --selftest =="
out=$("$BIN" --selftest 2>&1)
echo "$out" | grep -q "PASS" || { echo "FAIL: selftest did not PASS"; echo "$out"; exit 1; }
echo "$out" | grep -q "ch2: .* <-- target" || { echo "FAIL: channelizer self-test missing"; echo "$out"; exit 1; }
echo "ok"

echo "== Test 2: SigMF auto-config =="
TMPDIR=$(mktemp -d)
cat > "$TMPDIR/cap.cf32.sigmf-meta" <<EOF
{"global": {"core:datatype": "cf32_le", "core:sample_rate": 1000000},
 "captures": [{"core:sample_start": 0, "core:frequency": 906875000}]}
EOF
head -c 8000 /dev/urandom > "$TMPDIR/cap.cf32"
out=$("$BIN" --file="$TMPDIR/cap.cf32" --presets=LongFast 2>&1 || true)
echo "$out" | grep -q "sigmf:" || { echo "FAIL: sigmf metadata not picked up"; echo "$out"; rm -rf "$TMPDIR"; exit 1; }
echo "$out" | grep -q "906.875 MHz" || { echo "FAIL: sigmf frequency not applied"; echo "$out"; rm -rf "$TMPDIR"; exit 1; }
rm -rf "$TMPDIR"
echo "ok"

echo "== Test 3: --list =="
out=$("$BIN" --list 2>&1 || true)
echo "$out" | grep -q "Available SDR devices" || { echo "FAIL: --list missing header"; echo "$out"; exit 1; }
echo "ok"

echo "== Test 4: web /api/* round-trip =="
TMPDIR=$(mktemp -d)
head -c 1048576 /dev/urandom > "$TMPDIR/iq.cs8"
"$BIN" --file="$TMPDIR/iq.cs8" --rate=20000000 --center=910000000 \
       --presets=LongFast --web="$PORT" > "$TMPDIR/sniffer.log" 2>&1 &
PID=$!
trap "kill $PID 2>/dev/null; rm -rf $TMPDIR" EXIT
sleep 0.4

# /api/keys
r=$(curl -s -X POST -d 'Ops=hex:00112233445566778899aabbccddeeff' "http://127.0.0.1:$PORT/api/keys")
echo "$r" | grep -q '"added":1' || { echo "FAIL: /api/keys"; echo "$r"; exit 1; }

# /api/extra-freq
r=$(curl -s -X POST -d '915183000:bw=125000:sf=12:cr=8' "http://127.0.0.1:$PORT/api/extra-freq")
echo "$r" | grep -q '"channel_id"' || { echo "FAIL: /api/extra-freq"; echo "$r"; exit 1; }

# /api/cot-multicast (set, then disable)
r=$(curl -s -X POST -d '239.5.5.5:7000' "http://127.0.0.1:$PORT/api/cot-multicast")
echo "$r" | grep -q '"enabled":true' || { echo "FAIL: /api/cot-multicast set"; echo "$r"; exit 1; }
r=$(curl -s -X POST -d '' "http://127.0.0.1:$PORT/api/cot-multicast")
echo "$r" | grep -q '"enabled":false' || { echo "FAIL: /api/cot-multicast disable"; echo "$r"; exit 1; }

# GET / dashboard
r=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/")
[[ "$r" == "200" ]] || { echo "FAIL: GET / status=$r"; exit 1; }
echo "ok"

echo "== Test 5: spectrum SSE event =="
kill $PID 2>/dev/null; wait 2>/dev/null

head -c 33554432 /dev/urandom > "$TMPDIR/big.cs8"
"$BIN" --file="$TMPDIR/big.cs8" --rate=20000000 --center=910000000 \
       --presets=LongFast --web="$PORT" --web-spectrum > "$TMPDIR/sniffer.log" 2>&1 &
PID=$!
sleep 2.5
spec=$(timeout 1.5 curl -s -N "http://127.0.0.1:$PORT/events" | grep -o 'SPECTRUM' | head -1)
kill $PID 2>/dev/null; wait 2>/dev/null
[[ "$spec" == "SPECTRUM" ]] || { echo "FAIL: no SPECTRUM event from /events"; exit 1; }
echo "ok"

echo "== Test 6: stats heartbeat =="
"$BIN" --file="$TMPDIR/big.cs8" --rate=20000000 --center=910000000 \
       --presets=LongFast > "$TMPDIR/stats.log" 2>&1 &
PID=$!
sleep 6
kill $PID 2>/dev/null; wait 2>/dev/null
grep -q "\\[stats\\]" "$TMPDIR/stats.log" || { echo "FAIL: no stats heartbeat in 6s"; cat "$TMPDIR/stats.log"; exit 1; }
echo "ok"

echo
echo "ALL TESTS PASSED"
