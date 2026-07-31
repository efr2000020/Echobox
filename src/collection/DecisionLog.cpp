// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// DecisionLog implementation. See DecisionLog.hpp for the contract.

#include "DecisionLog.hpp"

#include "logging/Logger.hpp"

#include <cstdio>
#include <system_error>

namespace echobox::collection {

DecisionLog::DecisionLog(std::filesystem::path path)
    : m_path(std::move(path)) {
    std::error_code ec;
    std::filesystem::create_directories(m_path.parent_path(), ec);
    if (ec) {
        LS_WARN("collection", "decision-log: mkdir %s failed: %s",
                m_path.parent_path().string().c_str(), ec.message().c_str());
    }
    m_fp = std::fopen(m_path.string().c_str(), "wb");
    if (!m_fp) {
        LS_ERROR("collection", "decision-log: fopen %s failed",
                 m_path.string().c_str());
    }
}

DecisionLog::~DecisionLog() {
    if (m_fp) {
        std::fclose(static_cast<std::FILE*>(m_fp));
        m_fp = nullptr;
    }
}

bool DecisionLog::append(const std::string& line) {
    if (!m_fp) return false;
    std::lock_guard<std::mutex> lk(m_mutex);
    auto* fp = static_cast<std::FILE*>(m_fp);
    if (std::fwrite(line.data(), 1, line.size(), fp) != line.size()) return false;
    if (std::fputc('\n', fp) == EOF) return false;
    // fflush after every record so a crash mid-session leaves a
    // trustworthy tail. The plan (§2.3) treats partial sessions as
    // fully-trustworthy up to the last complete record.
    std::fflush(fp);
    return true;
}

void DecisionLog::flush() {
    if (!m_fp) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    std::fflush(static_cast<std::FILE*>(m_fp));
}

} // namespace echobox::collection
