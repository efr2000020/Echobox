#!/bin/bash
#
# run_debug.sh — debug-mode Echobox launch for false-positive investigations.
#
# Compared to run.sh this turns on verbose logging and stretches the recorder's
# pre/post-roll so each saved WAV carries enough ambient lead-in for the offline
# validator to seed its adaptive noise floor before reaching the trigger frame.
# Storage and battery are explicitly not considerations here.
#
# Detector parameters are left at production defaults on purpose: the point is
# to reproduce what the shipped detector decided, not to run a more sensitive
# variant. Override anything by passing extra flags after the script name.
#
#   ./run_debug.sh
#   ./run_debug.sh --snr-threshold 8.0      # extra flags pass through
#
# Knobs deviating from run.sh:
#   --log-level debug           Per-100-frame heartbeat + event start/end JSON
#   --preroll-ms 3000           ~2250 frames at hop=512 / 384 kHz; well past
#                               the 40-frame warmup gate and ~11x the EMA
#                               floor time constant (alpha_rise=0.995 -> tau
#                               ~200 frames ~270 ms), so the floor is fully
#                               converged before the trigger frame arrives
#   --silence-ms 5000           Keep recording 5 s past last hot frame; long
#                               enough to capture full call trains and the
#                               decay tail of any interferer
#   --max-length-ms 60000       60 s hard cap (was 5 s) so sustained sources
#                               (mechanical, electrical, insect) record long
#                               enough to be identified, instead of being
#                               truncated mid-event
#   --output-dir / --log-dir    Separate trees so debug captures don't mix
#                               with production data

set -euo pipefail

DEPLOY_EXE="./deploy/bin/Echobox"
DEVICE="${ECHOBOX_DEVICE:-plughw:UltraMic384K,1,0}"

if [ ! -x "$DEPLOY_EXE" ]; then
    echo "Error: $DEPLOY_EXE not found. Run ./build_and_deploy.sh first." >&2
    exit 1
fi

mkdir -p ./recordings_debug ./logs_debug

echo "--- Launching Echobox [DEBUG] (device=$DEVICE) ---"
echo "    log-level=debug  preroll=3000ms  silence=5000ms  max-length=60000ms"
echo "    output -> ./recordings_debug    logs -> ./logs_debug"

exec "$DEPLOY_EXE" \
    --device "$DEVICE" \
    --log-level debug \
    --log-dir ./logs_debug \
    --output-dir ./recordings_debug \
    --preroll-ms 3000 \
    --silence-ms 5000 \
    --max-length-ms 60000 \
    "$@"
