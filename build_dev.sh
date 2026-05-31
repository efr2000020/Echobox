#!/bin/bash
set -e

# build_dev.sh
# Builds the project in Debug mode with dynamic plugins enabled for development.

PROJECT_ROOT=$(pwd)
BUILD_DIR="${PROJECT_ROOT}/build"
DEPLOY_DIR="${PROJECT_ROOT}/deploy"

echo "--- Starting Development Build ---"

# Create build directory if it doesn't exist
mkdir -p "$BUILD_DIR"

# Configure
# Debug mode + Dynamic Plugins enabled.
BUILD_TYPE=Debug
echo "[1/3] Configuring with CMake (Ninja) in ${BUILD_TYPE} mode (Dynamic Plugins ON)..."
cmake -S . -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_INSTALL_PREFIX="$DEPLOY_DIR" \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DECHOBOX_DYNAMIC_PLUGINS=ON

# Build
echo "[2/3] Building..."
cmake --build "$BUILD_DIR" -j1

# Install/Deploy
echo "[3/3] Installing to $DEPLOY_DIR..."
cmake --install "$BUILD_DIR"

echo "--- Dev Build Successful ---"
echo "You can run the application using: ./run.sh"
