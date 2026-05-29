add_executable(unit_tests
    test_LockFreeRingBuffer.cpp
)

target_link_libraries(unit_tests
    PRIVATE
        Catch2::Catch2WithMain
        ls_common
)

include(Catch)
catch_discover_tests(unit_tests)
