#!/bin/bash
set -e

# build_and_deploy.sh
# Builds the project and installs it to the 'deploy' folder.

PROJECT_ROOT=$(pwd)
BUILD_DIR="${PROJECT_ROOT}/build"
DEPLOY_DIR="${PROJECT_ROOT}/deploy"

echo "--- Starting Build and Deploy ---"

# Create build directory if it doesn't exist
mkdir -p "$BUILD_DIR"

# Configure
BUILD_TYPE=${1:-Debug}
echo "[1/3] Configuring with CMake (Ninja) in ${BUILD_TYPE} mode..."
cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_INSTALL_PREFIX="$DEPLOY_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

# Build
# Single job: the Pi Zero 2 W (512 MB RAM) OOM-kills cc1plus under parallel
# C++ compiles. We rarely rebuild, so favor reliability over speed.
echo "[2/3] Building..."
cmake --build "$BUILD_DIR" -j1

# Install/Deploy
echo "[3/3] Installing to $DEPLOY_DIR..."
cmake --install "$BUILD_DIR"

echo "--- Build and Deploy Successful ---"
echo "You can run the application using: ./run.sh"
