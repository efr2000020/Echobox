#!/bin/bash

# clean.sh
# Cleans the build and deploy folders.

PROJECT_ROOT=$(pwd)
BUILD_DIR="${PROJECT_ROOT}/build"
DEPLOY_DIR="${PROJECT_ROOT}/deploy"

echo "--- Cleaning Project ---"

if [ -d "$BUILD_DIR" ]; then
    echo "Removing $BUILD_DIR..."
    rm -rf "$BUILD_DIR"
fi

if [ -d "$DEPLOY_DIR" ]; then
    echo "Removing $DEPLOY_DIR..."
    rm -rf "$DEPLOY_DIR"
fi

echo "--- Clean Complete ---"
