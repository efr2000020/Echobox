#!/bin/bash
#
# run.sh — launch Echobox from ./deploy for local validation.
#
# Defaults match the shipped Config: capture from the UltraMic384K, logging
# off. Override the capture device with the ECHOBOX_DEVICE environment
# variable, and pass any extra Echobox options as arguments — they go through
# verbatim, so e.g. `--log-level debug` opts into the per-frame diagnostics
# when you're debugging locally.
#
#   ./run.sh                                              # silent, prod defaults
#   ECHOBOX_DEVICE=plughw:CARD=UltraMic384K ./run.sh
#   ./run.sh --log-level debug                            # opt into logging
#   ./run.sh --output-dir /tmp/rec --log-level info       # any flags pass through
#
set -euo pipefail

DEPLOY_EXE="./deploy/bin/Echobox"
DEVICE="${ECHOBOX_DEVICE:-plughw:UltraMic384K,1,0}"

if [ ! -x "$DEPLOY_EXE" ]; then
    echo "Error: $DEPLOY_EXE not found. Run ./build_and_deploy.sh first." >&2
    exit 1
fi

echo "--- Launching Echobox (device=$DEVICE) ---"
exec "$DEPLOY_EXE" --device "$DEVICE" "$@"
