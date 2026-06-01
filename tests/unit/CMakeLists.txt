add_executable(unit_tests
    test_LockFreeRingBuffer.cpp
    test_ConfigValidator.cpp
    test_CliParser.cpp
    test_BiquadFilter.cpp
    test_PreRollBuffer.cpp
    test_LogQueue.cpp
)

target_link_libraries(unit_tests
    PRIVATE
        Catch2::Catch2WithMain
        ls_common
        ls_app
        ls_recorder
        ls_logging
)

include(Catch)
catch_discover_tests(unit_tests)
