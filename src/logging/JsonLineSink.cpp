/// @file
/// JsonLineSink implementation. See JsonLineSink.hpp for the contract.

#include "JsonLineSink.hpp"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace echobox::logging {

namespace {

void appendEscapedJson(std::string& out, const char* s, std::size_t maxLen) {
    for (std::size_t i = 0; i < maxLen && s[i] != '\0'; ++i) {
        char c = s[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
}

void formatTimestamp(std::chrono::system_clock::time_point tp, char* out, std::size_t cap) {
    using namespace std::chrono;
    const auto secs = time_point_cast<seconds>(tp);
    const auto millis = duration_cast<milliseconds>(tp - secs).count();
    const auto t = system_clock::to_time_t(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long long>(millis));
}

} // namespace

JsonLineSink::JsonLineSink(fs::path dir, std::string baseName,
                           std::size_t maxBytes, unsigned int keep)
    : m_dir(std::move(dir)),
      m_baseName(std::move(baseName)),
      m_maxBytes(maxBytes),
      m_keep(keep),
      m_currentPath(m_dir / m_baseName)
{
    std::error_code ec;
    fs::create_directories(m_dir, ec);
    if (ec) {
        throw std::runtime_error("logger: failed to create log dir: " + ec.message());
    }
    openCurrent();
}

JsonLineSink::~JsonLineSink() {
    if (m_fp) {
        std::fflush(m_fp);
        std::fclose(m_fp);
    }
}

void JsonLineSink::openCurrent() {
    m_fp = std::fopen(m_currentPath.c_str(), "ab");
    if (!m_fp) {
        throw std::runtime_error("logger: cannot open " + m_currentPath.string()
                                 + ": " + std::strerror(errno));
    }
    std::error_code ec;
    m_currentBytes = static_cast<std::size_t>(fs::file_size(m_currentPath, ec));
    if (ec) m_currentBytes = 0;
}

void JsonLineSink::rotateIfNeeded(std::size_t pendingBytes) {
    if (m_currentBytes + pendingBytes > m_maxBytes) {
        rotateNow();
    }
}

void JsonLineSink::rotateNow() {
    if (m_fp) {
        std::fflush(m_fp);
        std::fclose(m_fp);
        m_fp = nullptr;
    }

    // Shift baseName.<keep-1> -> deleted, ... baseName.1 -> baseName.2, baseName -> baseName.1
    for (unsigned int i = m_keep; i >= 1; --i) {
        fs::path from = m_dir / (m_baseName + "." + std::to_string(i - 1));
        fs::path to   = m_dir / (m_baseName + "." + std::to_string(i));
        if (i - 1 == 0) from = m_currentPath;

        std::error_code ec;
        if (i == m_keep) {
            if (fs::exists(to, ec)) fs::remove(to, ec);
        }
        if (fs::exists(from, ec)) {
            fs::rename(from, to, ec);
        }
    }

    m_currentBytes = 0;
    openCurrent();
}

void JsonLineSink::write(const LogRecord& r) {
    char tsBuf[40];
    formatTimestamp(r.ts, tsBuf, sizeof(tsBuf));

    std::string line;
    line.reserve(LogRecord::MESSAGE_MAX + 96);
    line += "{\"ts\":\"";
    line += tsBuf;
    line += "\",\"level\":\"";
    line += levelName(r.level);
    line += "\",\"subsystem\":\"";
    appendEscapedJson(line, r.subsystem, LogRecord::SUBSYSTEM_MAX);
    line += "\",\"msg\":\"";
    appendEscapedJson(line, r.message, LogRecord::MESSAGE_MAX);
    line += "\"}\n";

    rotateIfNeeded(line.size());
    std::fwrite(line.data(), 1, line.size(), m_fp);
    m_currentBytes += line.size();
}

void JsonLineSink::writeDropped(std::size_t droppedCount) {
    LogRecord r{};
    r.ts    = std::chrono::system_clock::now();
    r.level = LogLevel::Warn;
    std::snprintf(r.subsystem, LogRecord::SUBSYSTEM_MAX, "%s", "logger");
    std::snprintf(r.message, LogRecord::MESSAGE_MAX,
                  "dropped %zu log records (queue full)", droppedCount);
    write(r);
}

void JsonLineSink::flush() {
    if (m_fp) std::fflush(m_fp);
}

} // namespace echobox::logging
