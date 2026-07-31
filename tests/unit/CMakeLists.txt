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
    test_Session.cpp
    test_ContinuousWriter.cpp
    test_DecisionLog.cpp
)

target_link_libraries(unit_tests
    PRIVATE
        Catch2::Catch2WithMain
        ls_common
        ls_app
        ls_recorder
        ls_collection
        ls_logging
        BandEnergyDetector
)

include(Catch)
catch_discover_tests(unit_tests)
