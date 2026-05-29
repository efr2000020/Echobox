add_library(ls_common STATIC
    SignalHandler.cpp
)
target_include_directories(ls_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ls_common PUBLIC Threads::Threads)

# Header-only neighbours exposed via the same include path.
target_sources(ls_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/LockFreeRingBuffer.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PathUtils.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SignalHandler.hpp
)
