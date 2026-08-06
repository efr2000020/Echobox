// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// RunManifest implementation. See RunManifest.hpp.

#include "RunManifest.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace echobox::replay {

namespace {

/// Escape a string for JSON. Handles the printable-ASCII edge cases the
/// paths + configs we emit can hit; not a full RFC 8259 escaper.
std::string esc(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

const char* saveRejectedName(echobox::app::Config::SaveRejectedMode m) {
    using M = echobox::app::Config::SaveRejectedMode;
    switch (m) {
        case M::Off:      return "off";
        case M::All:      return "all";
        case M::Sample:   return "sample";
        case M::Boundary: return "boundary";
    }
    return "off";
}

} // namespace

void writeRunManifest(const fs::path& path, const RunManifest& m) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    std::ostringstream os;
    os << "{\n";
    os << "  \"tool\":         \"echobox-replay\",\n";
    os << "  \"tool_version\": \"" << esc(m.tool_version) << "\",\n";
    os << "  \"platform\":     \"" << esc(m.platform)     << "\",\n";
    os << "  \"build_type\":   \"" << esc(m.build_type)   << "\",\n";
    os << "  \"fast_math\":    " << (m.fast_math ? "true" : "false") << ",\n";
    os << "  \"compiler\":     \"" << esc(m.compiler)     << "\",\n";
    os << "  \"run_ts\":       \"" << esc(m.run_iso8601)  << "\",\n";
    os << "  \"input\":        \"" << esc(m.input.string())  << "\",\n";
    os << "  \"output\":       \"" << esc(m.output.string()) << "\",\n";
    os << "  \"config_source\":\"" << esc(m.config_source) << "\",\n";

    os << "  \"cli_argv\": [";
    for (std::size_t i = 0; i < m.cli_argv.size(); ++i) {
        if (i) os << ", ";
        os << "\"" << esc(m.cli_argv[i]) << "\"";
    }
    os << "],\n";

    // Effective config — what the pipeline actually ran with.
    const auto& c = m.config;
    os << "  \"config\": {\n";
    os << "    \"sample_rate\":     " << c.sampleRate    << ",\n";
    os << "    \"channels\":        " << c.channels      << ",\n";
    os << "    \"algorithm\":       \"" << esc(c.algorithm) << "\",\n";
    os << "    \"fft_size\":        " << c.fftSize       << ",\n";
    os << "    \"hop_size\":        " << c.hopSize       << ",\n";
    os << "    \"freq_lo_hz\":      " << c.freqLoHz      << ",\n";
    os << "    \"freq_hi_hz\":      " << c.freqHiHz      << ",\n";
    os << "    \"snr_threshold\":   " << c.snrThreshold  << ",\n";
    os << "    \"preroll_ms\":      " << c.preRollMs     << ",\n";
    os << "    \"silence_ms\":      " << c.silenceMs     << ",\n";
    os << "    \"min_length_ms\":   " << c.minLengthMs   << ",\n";
    os << "    \"max_length_ms\":   " << c.maxLengthMs   << ",\n";
    os << "    \"cricket_filter\":  " << (c.cricketFilter ? "true" : "false") << ",\n";
    os << "    \"save_rejected\":   \"" << saveRejectedName(c.saveRejected) << "\",\n";
    os << "    \"save_rejected_sample_n\":     " << c.saveRejectedSampleN     << ",\n";
    os << "    \"save_rejected_max_per_hour\": " << c.saveRejectedMaxPerHour  << "\n";
    os << "  }";

    if (m.had_session_header) {
        // Round-trip the header slice so a reader can see the exact source
        // of truth we resolved config against, not just our interpretation.
        const auto& h = m.session_header;
        os << ",\n  \"session_header\": {\n";
        os << "    \"source\": \"" << esc(h.source.string()) << "\"";
        auto emitOptI = [&](const char* k, const auto& opt) {
            if (opt) os << ",\n    \"" << k << "\": " << *opt;
        };
        auto emitOptS = [&](const char* k, const std::optional<std::string>& opt) {
            if (opt) os << ",\n    \"" << k << "\": \"" << esc(*opt) << "\"";
        };
        auto emitOptB = [&](const char* k, const std::optional<bool>& opt) {
            if (opt) os << ",\n    \"" << k << "\": " << (*opt ? "true" : "false");
        };
        emitOptI("sample_rate",    h.sample_rate);
        emitOptI("channels",       h.channels);
        emitOptS("algorithm",      h.algorithm);
        emitOptI("preroll_ms",     h.preroll_ms);
        emitOptI("silence_ms",     h.silence_ms);
        emitOptI("min_length_ms",  h.min_length_ms);
        emitOptI("max_length_ms",  h.max_length_ms);
        emitOptI("snr_threshold",  h.snr_threshold);
        emitOptI("fft_size",       h.fft_size);
        emitOptI("hop_size",       h.hop_size);
        emitOptI("freq_lo_hz",     h.freq_lo_hz);
        emitOptI("freq_hi_hz",     h.freq_hi_hz);
        emitOptB("cricket_filter", h.cricket_filter);
        os << "\n  }";
    }

    os << "\n}\n";

    // Atomic write via temp + rename — matches Sidecar's convention so a
    // reader that finds the file will never see a half-written manifest.
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << os.str();
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        // Fall back to copy+remove for cross-fs writes.
        fs::copy_file(tmp, path, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp, ec);
    }
}

} // namespace echobox::replay
