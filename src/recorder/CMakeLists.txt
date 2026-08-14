# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(ls_recorder STATIC
    PreRollBuffer.cpp
    WavWriter.cpp
    FilenameBuilder.cpp
    Recorder.cpp
    Sidecar.cpp
)
target_include_directories(ls_recorder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(ls_recorder PRIVATE ${SNDFILE_INCLUDE_DIRS})
target_link_libraries(ls_recorder
    PUBLIC  ls_logging Threads::Threads
    PRIVATE ${SNDFILE_LIBRARIES}
)

# Recorder clock-injection seam (RecorderClock.hpp). Compiled OUT of the
# field build, so the poll loop provably pays nothing for a feature only
# workstation tooling uses: deploy/bin/Echobox builds to the same
# instruction count and the same section sizes as the pre-seam binary
# (the one residual difference is documented in RecorderClock.hpp).
# Compiled IN for the unit tests and for
# echobox-replay — the only two consumers that ever set
# RecorderConfig::clock. PUBLIC because the define changes the layout of
# RecorderConfig, so every TU in a given build must agree on it.
if(ECHOBOX_BUILD_REPLAY OR ECHOBOX_BUILD_TESTS)
    target_compile_definitions(ls_recorder PUBLIC ECHOBOX_RECORDER_CLOCK_INJECTION=1)
endif()
