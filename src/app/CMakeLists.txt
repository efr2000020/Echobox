# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(ls_app STATIC
    CliParser.cpp
    ConfigValidator.cpp
    Application.cpp
)
target_include_directories(ls_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ls_app
    PUBLIC  ls_logging ls_common ls_audio ls_dsp_pipeline ls_recorder ls_collection dsp_interface
    PRIVATE Threads::Threads
)

if(ECHOBOX_DYNAMIC_PLUGINS)
    # Dev: detectors are dlopen'd at runtime; the app scans ./algorithms.
    target_compile_definitions(ls_app PRIVATE ECHOBOX_DYNAMIC_PLUGINS=1)
else()
    # Release: the detector is compiled in and registered directly.
    target_link_libraries(ls_app PUBLIC BandEnergyDetector)
endif()
