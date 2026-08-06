# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(unit_tests
    test_LockFreeRingBuffer.cpp
    test_ConfigValidator.cpp
    test_CliParser.cpp
    test_BiquadFilter.cpp
    test_PreRollBuffer.cpp
    test_LogQueue.cpp
    test_Sidecar.cpp
    test_SweepShape.cpp
    test_RepStats.cpp
    test_Recorder.cpp
    test_ConsoleSink.cpp
)

target_link_libraries(unit_tests
    PRIVATE
        Catch2::Catch2WithMain
        ls_common
        ls_app
        ls_recorder
        ls_logging
        BandEnergyDetector
)

# The WavFileAudioSource lives in tools/replay/ (workstation-only). Its
# test compiles only when the replay tool is being built, so tests/unit/
# does not depend on the replay target existing in the default build.
if(ECHOBOX_BUILD_REPLAY)
    target_sources(unit_tests PRIVATE
        test_WavFileAudioSource.cpp
        test_SessionHeader.cpp
    )
    target_link_libraries(unit_tests PRIVATE ls_replay_wav ${SNDFILE_LIBRARIES})
    target_include_directories(unit_tests PRIVATE ${SNDFILE_INCLUDE_DIRS})
endif()

include(Catch)
catch_discover_tests(unit_tests)
