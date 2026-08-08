# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)
project(Echobox VERSION 0.3.0 LANGUAGES C CXX)
# 0.3.0-rc1 — veto-recovery pre-release. Temporal rep-guard disabled by
# default (recovers ~2500 bat calls/night on Pipistrellus corpora at
# the cost of ~30% relative cricket-FP increase). Re-enable in-config
# with ``rep_guard_enabled=1`` for CF/Rhinolophus sites. See CHANGELOG.md.
set(ECHOBOX_PRERELEASE "rc1")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Export compile commands for IDE support.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build-time options.
option(ECHOBOX_BUILD_TESTS "Build unit tests (fetches Catch2 over the network)" OFF)
option(ECHOBOX_DYNAMIC_PLUGINS
       "Load detectors as hot-swappable .so plugins from ./algorithms (dev). OFF statically links the detector into a single binary (release)."
       OFF)
# Off-tree offline validation tool. OFF ⇒ tools/replay/ contributes no
# sources, targets, or install artefacts — the shipping binary and its
# install layout are byte-identical to a tree without the replay dir.
option(ECHOBOX_BUILD_REPLAY
       "Build the echobox-replay offline validation tool (workstation-only). OFF ships nothing from tools/replay/."
       OFF)

# Optimization / debug flags.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -march=native -ffast-math)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0)
endif()

include(FetchContent)

# KissFFT (production FFT backend; backend swap is at the IFftEngine layer).
set(KISSFFT_STATIC ON  CACHE BOOL "")
set(KISSFFT_TEST   OFF CACHE BOOL "")
set(KISSFFT_TOOLS  OFF CACHE BOOL "")
FetchContent_Declare(
    kissfft
    GIT_REPOSITORY https://github.com/mborgerding/kissfft.git
    GIT_TAG        131.1.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(kissfft)

# Catch2 (unit tests). Only fetched if tests are enabled.
if(ECHOBOX_BUILD_TESTS)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.4.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)
endif()

add_subdirectory(src)

if(ECHOBOX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
endif()

# Replay tool — workstation-only, off by default. Kept behind a build-time
# gate rather than #ifdefs in core code, so the shipping tree stays clean.
if(ECHOBOX_BUILD_REPLAY)
    add_subdirectory(tools/replay)
endif()

# Install layout: binary, algorithm plugins.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/deploy" CACHE PATH "Default install path" FORCE)
endif()

install(TARGETS Echobox RUNTIME DESTINATION bin)

# Status summary.
message(STATUS "Echobox config:")
message(STATUS "  build type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  dynamic plugins   : ${ECHOBOX_DYNAMIC_PLUGINS}")
message(STATUS "  tests             : ${ECHOBOX_BUILD_TESTS}")
message(STATUS "  replay tool       : ${ECHOBOX_BUILD_REPLAY}")
