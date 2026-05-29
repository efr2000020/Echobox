add_library(ls_logging STATIC
    LogRecord.cpp
    LogQueue.cpp
    JsonLineSink.cpp
    Logger.cpp
)
target_include_directories(ls_logging PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ls_logging PUBLIC Threads::Threads)
