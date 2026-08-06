#!/bin/bash
set -e

# build_dev.sh
# Builds the project in Debug mode with dynamic plugins enabled for development.
#
# Options:
#   --replay    Also build the workstation-only echobox-replay tool
#               (off by default; the shipping app is unchanged either way).

PROJECT_ROOT=$(pwd)
BUILD_DIR="${PROJECT_ROOT}/build"
DEPLOY_DIR="${PROJECT_ROOT}/deploy"

BUILD_REPLAY=OFF
for arg in "$@"; do
    case "$arg" in
        --replay) BUILD_REPLAY=ON ;;
        -h|--help)
            cat <<EOF
Usage: $0 [--replay]

  --replay   Also build tools/replay/echobox-replay (workstation-only offline
             validation tool). The shipping Echobox binary is byte-identical
             with or without this flag.
EOF
            exit 0
            ;;
        *)
            echo "unknown option: $arg" >&2
            exit 2
            ;;
    esac
done

echo "--- Starting Development Build ---"

# Create build directory if it doesn't exist
mkdir -p "$BUILD_DIR"

# Configure
# Debug mode + Dynamic Plugins enabled.
BUILD_TYPE=Debug
echo "[1/3] Configuring with CMake (Ninja) in ${BUILD_TYPE} mode (Dynamic Plugins ON, Replay=${BUILD_REPLAY})..."
cmake -S . -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_INSTALL_PREFIX="$DEPLOY_DIR" \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DECHOBOX_DYNAMIC_PLUGINS=ON \
      -DECHOBOX_BUILD_REPLAY="${BUILD_REPLAY}"

# Build
echo "[2/3] Building..."
cmake --build "$BUILD_DIR" -j1

# Install/Deploy
echo "[3/3] Installing to $DEPLOY_DIR..."
cmake --install "$BUILD_DIR"

echo "--- Dev Build Successful ---"
echo "You can run the application using: ./run.sh"
if [ "$BUILD_REPLAY" = "ON" ]; then
    echo "Replay tool installed at: ${DEPLOY_DIR}/bin/echobox-replay"
fi
