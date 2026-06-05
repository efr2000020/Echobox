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
