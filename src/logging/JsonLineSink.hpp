#pragma once
/// @file
/// File sink for the Logger: JSON-lines with size-based rotation.

#include "LogRecord.hpp"
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>

namespace echobox::logging {

/**
 * @brief Appends @c LogRecord values as JSON-lines to a rotating file.
 *
 * Rotation: when the active file reaches @c maxBytes, it is renamed with a
 * @c .1 suffix (older @c .N files shifted up to @c .keep) and a fresh file
 * is opened.
 *
 * @note Not thread-safe. All I/O happens on the @c Logger consumer thread.
 */
class JsonLineSink {
public:
    JsonLineSink(std::filesystem::path dir,
                 std::string  baseName,
                 std::size_t  maxBytes,
                 unsigned int keep);
    ~JsonLineSink();

    JsonLineSink(const JsonLineSink&)            = delete;
    JsonLineSink& operator=(const JsonLineSink&) = delete;

    void write(const LogRecord& r);
    /// Surface drops from the upstream queue as a synthetic record so the
    /// loss is visible in the file.
    void writeDropped(std::size_t droppedCount);
    void flush();

private:
    void  openCurrent();
    void  rotateIfNeeded(std::size_t pendingBytes);
    void  rotateNow();

    std::filesystem::path m_dir;
    std::string           m_baseName;
    std::size_t           m_maxBytes;
    unsigned int          m_keep;

    std::filesystem::path m_currentPath;
    std::FILE*            m_fp{nullptr};
    std::size_t           m_currentBytes{0};
};

} // namespace echobox::logging
