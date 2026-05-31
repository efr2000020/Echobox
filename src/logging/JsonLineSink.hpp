#pragma once
#include "LogRecord.hpp"
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>

namespace echobox::logging {

// Appends LogRecords as JSON-lines to a rotating file under `dir`.
// Rotation: when the active file reaches `maxBytes`, it is renamed with a .1
// suffix (older .N files shifted up to .keep) and a fresh file is opened.
//
// All I/O happens on the LoggerThread; the sink itself is not thread-safe.
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
    // Surfaces drops from the upstream queue as a synthetic record so the loss
    // is visible in the file.
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
