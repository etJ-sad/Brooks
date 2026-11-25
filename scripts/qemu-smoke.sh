#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Headless smoke test: boot the image in QEMU+OVMF, wait for the (Phase 0 gate|KERNEL PANIC)
# line on the serial port, save a screenshot (PPM) and stop QEMU.
# Usage: qemu-smoke.sh <image> [screenshot.ppm] [timeout_s]
set -euo pipefail
cd "$(dirname "$0")/.."
IMG="$1"
SHOT="${2:-build/screen.ppm}"
TMO="${3:-120}"
LOG=build/serial.log
SOCK=$(mktemp -u /tmp/qemu-mon.XXXXXX)
mkdir -p build
: > "$LOG"

HEADLESS=1 ./scripts/run-qemu.sh "$IMG" -monitor unix:"$SOCK",server,nowait > "$LOG" 2>&1 &
QPID=$!
trap 'kill $QPID 2>/dev/null || true; rm -f "$SOCK"' EXIT

ok=0
for _ in $(seq 1 "$TMO"); do
    if grep -qE "Phase 0 gate|KERNEL PANIC" "$LOG"; then ok=1; break; fi
    kill -0 $QPID 2>/dev/null || break
    sleep 1
done
sleep 1
python3 - "$SOCK" "$(realpath -m "$SHOT")" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); time.sleep(0.3); s.recv(65536)
s.send(f"screendump {sys.argv[2]}\n".encode()); time.sleep(3)
s.send(b"quit\n"); time.sleep(0.5)
PY
sed -e 's/\x1b\[[0-9;=?]*[a-zA-Z]//g' "$LOG" | grep -v '^\s*$' | tail -n 60
if [[ $ok == 1 ]] && ! grep -q "KERNEL PANIC" "$LOG"; then echo "SMOKE: OK (screenshot $SHOT)"; else echo "SMOKE: FAILED (kernel panic or no gate line within ${TMO}s)"; exit 1; fi
