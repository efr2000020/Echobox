#!/bin/bash
#
# run.sh — launch Echobox from ./deploy for local validation.
#
# Defaults to the snd-aloop virtual microphone (see tools/fake-mic/) and
# maximum logging (--log-level debug). Override the capture device with the
# ECHOBOX_DEVICE environment variable, and pass any extra Echobox
# options as arguments — they take precedence over the defaults below.
#
#   ./run.sh                                  # loopback mic, debug logging
#   ECHOBOX_DEVICE=plughw:CARD=UltraMic384K ./run.sh
#   ./run.sh --output-dir /tmp/rec            # extra options pass through
#
set -euo pipefail

DEPLOY_EXE="./deploy/bin/Echobox"
DEVICE="${ECHOBOX_DEVICE:-plughw:UltraMic384K,1,0}"

if [ ! -x "$DEPLOY_EXE" ]; then
    echo "Error: $DEPLOY_EXE not found. Run ./build_and_deploy.sh first." >&2
    exit 1
fi

echo "--- Launching Echobox (device=$DEVICE, log-level=debug) ---"
exec "$DEPLOY_EXE" --device "$DEVICE" --log-level debug "$@"
