// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// SessionHeader implementation. See SessionHeader.hpp for the design note.
///
/// The extractor operates on a whitespace-normalised copy of the file: all
/// runs of whitespace collapse to a single space, so we can search for
/// `"key": value` reliably even if the writer changes its indentation.
/// This is not a JSON parser — it does not handle escapes inside string
/// values or nested objects other than the flat "config" body. That's a
/// deliberate scope choice; see the header.

#include "SessionHeader.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using echobox::app::Config;

namespace echobox::replay {

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    if (!in) throw std::runtime_error("session-header: cannot open " + p.string());
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

std::string normalise(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    bool prevSpace = false;
    for (char c : raw) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!prevSpace) out.push_back(' ');
            prevSpace = true;
        } else {
            out.push_back(c);
            prevSpace = false;
        }
    }
    return out;
}

/// Locate the value token for `"key"` in @p body. Returns the value view
/// (still containing quotes for strings, digits for numbers, `true`/`false`
/// for booleans) or empty if the key isn't present.
std::string_view findValue(std::string_view body, std::string_view key) {
    std::string needle = "\"";
    needle.append(key);
    needle.append("\":");
    auto pos = body.find(needle);
    if (pos == std::string_view::npos) {
        // Tolerate a single space after the colon in unnormalised bodies —
        // callers pass normalised bodies today, but the double-lookup keeps
        // the extractor honest.
        needle = "\"";
        needle.append(key);
        needle.append("\": ");
        pos = body.find(needle);
        if (pos == std::string_view::npos) return {};
    }
    pos += needle.size();
    while (pos < body.size() && body[pos] == ' ') ++pos;
    if (pos >= body.size()) return {};

    // Scan to the end of the value: comma or closing brace at the same
    // nesting level. Values in our headers are never nested (numbers,
    // booleans, strings), so a naive scan is safe.
    std::size_t end = pos;
    if (body[end] == '"') {
        ++end;
        while (end < body.size() && body[end] != '"') ++end;
        if (end < body.size()) ++end; // include closing quote
    } else {
        while (end < body.size() && body[end] != ',' && body[end] != '}') ++end;
        // trim trailing space
        while (end > pos && body[end - 1] == ' ') --end;
    }
    return body.substr(pos, end - pos);
}

/// Strip surrounding quotes off a string-literal token, if any.
std::string unquote(std::string_view v) {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        return std::string(v.substr(1, v.size() - 2));
    }
    return std::string(v);
}

std::optional<int> asInt(std::string_view v) {
    if (v.empty()) return std::nullopt;
    std::string s(v);
    char* end = nullptr;
    errno = 0;
    long x = std::strtol(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str()) return std::nullopt;
    return static_cast<int>(x);
}
std::optional<std::uint32_t> asUint(std::string_view v) {
    auto i = asInt(v);
    if (!i || *i < 0) return std::nullopt;
    return static_cast<std::uint32_t>(*i);
}
std::optional<std::size_t> asSize(std::string_view v) {
    auto i = asInt(v);
    if (!i || *i < 0) return std::nullopt;
    return static_cast<std::size_t>(*i);
}
std::optional<float> asFloat(std::string_view v) {
    if (v.empty()) return std::nullopt;
    std::string s(v);
    char* end = nullptr;
    errno = 0;
    float x = std::strtof(s.c_str(), &end);
    if (errno != 0 || end == s.c_str()) return std::nullopt;
    return x;
}
std::optional<bool> asBool(std::string_view v) {
    if (v == "true"  || v == "1") return true;
    if (v == "false" || v == "0") return false;
    return std::nullopt;
}

/// Locate the flat "config" object inside a normalised body and return the
/// slice between the outermost `{` and matching `}`. Empty if no config
/// object is present.
std::string_view sliceConfig(std::string_view body) {
    std::string_view needle = "\"config\":";
    auto pos = body.find(needle);
    if (pos == std::string_view::npos) return {};
    pos += needle.size();
    while (pos < body.size() && body[pos] == ' ') ++pos;
    if (pos >= body.size() || body[pos] != '{') return {};
    ++pos;
    std::size_t depth = 1;
    std::size_t end = pos;
    while (end < body.size() && depth > 0) {
        if (body[end] == '{') ++depth;
        else if (body[end] == '}') {
            --depth;
            if (depth == 0) break;
        }
        ++end;
    }
    if (depth != 0) return {};
    return body.substr(pos, end - pos);
}

} // namespace

fs::path findSessionHeader(const fs::path& inputPath) {
    fs::path dir = fs::is_directory(inputPath) ? inputPath : inputPath.parent_path();
    // Walk up to 4 parent levels — covers collection_.../collection/reference/
    // and similar layouts without escaping to the filesystem root.
    for (int i = 0; i < 5; ++i) {
        if (dir.empty()) break;
        fs::path candidate = dir / "SESSION_HEADER.json";
        std::error_code ec;
        if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
        fs::path parent = dir.parent_path();
        if (parent == dir) break; // hit root
        dir = parent;
    }
    return {};
}

SessionHeaderConfig readSessionHeader(const fs::path& jsonPath) {
    SessionHeaderConfig h;
    h.source = jsonPath;

    const std::string raw  = slurp(jsonPath);
    const std::string body = normalise(raw);
    const std::string_view bodyView(body);

    // Top-level fields.
    if (auto v = findValue(bodyView, "sample_rate"); !v.empty()) h.sample_rate = asInt(v);
    if (auto v = findValue(bodyView, "channels");    !v.empty()) h.channels    = asInt(v);
    if (auto v = findValue(bodyView, "algorithm");   !v.empty()) h.algorithm   = unquote(v);

    // The nested "config" object carries the recorder/detector tunables.
    // Isolating the slice keeps our flat search from matching keys of the
    // same name that might appear elsewhere in the file (e.g. in "governor").
    auto cfgSlice = sliceConfig(bodyView);
    if (!cfgSlice.empty()) {
        if (auto v = findValue(cfgSlice, "preroll_ms");     !v.empty()) h.preroll_ms     = asUint(v);
        if (auto v = findValue(cfgSlice, "silence_ms");     !v.empty()) h.silence_ms     = asUint(v);
        if (auto v = findValue(cfgSlice, "min_length_ms");  !v.empty()) h.min_length_ms  = asUint(v);
        if (auto v = findValue(cfgSlice, "max_length_ms");  !v.empty()) h.max_length_ms  = asUint(v);
        if (auto v = findValue(cfgSlice, "snr_threshold");  !v.empty()) h.snr_threshold  = asFloat(v);
        if (auto v = findValue(cfgSlice, "fft_size");       !v.empty()) h.fft_size       = asSize(v);
        if (auto v = findValue(cfgSlice, "hop_size");       !v.empty()) h.hop_size       = asSize(v);
        if (auto v = findValue(cfgSlice, "freq_lo_hz");     !v.empty()) h.freq_lo_hz     = asInt(v);
        if (auto v = findValue(cfgSlice, "freq_hi_hz");     !v.empty()) h.freq_hi_hz     = asInt(v);
        if (auto v = findValue(cfgSlice, "cricket_filter"); !v.empty()) h.cricket_filter = asBool(v);
    }
    return h;
}

std::size_t applySessionHeader(const SessionHeaderConfig& hdr, Config& cfg) {
    std::size_t applied = 0;
    if (hdr.sample_rate)    { cfg.sampleRate    = *hdr.sample_rate;    ++applied; }
    if (hdr.channels)       { cfg.channels      = *hdr.channels;       ++applied; }
    if (hdr.algorithm)      { cfg.algorithm     = *hdr.algorithm;      ++applied; }
    if (hdr.preroll_ms)     { cfg.preRollMs     = *hdr.preroll_ms;     ++applied; }
    if (hdr.silence_ms)     { cfg.silenceMs     = *hdr.silence_ms;     ++applied; }
    if (hdr.min_length_ms)  { cfg.minLengthMs   = *hdr.min_length_ms;  ++applied; }
    if (hdr.max_length_ms)  { cfg.maxLengthMs   = *hdr.max_length_ms;  ++applied; }
    if (hdr.snr_threshold)  { cfg.snrThreshold  = *hdr.snr_threshold;  ++applied; }
    if (hdr.fft_size)       { cfg.fftSize       = *hdr.fft_size;       ++applied; }
    if (hdr.hop_size)       { cfg.hopSize       = *hdr.hop_size;       ++applied; }
    if (hdr.freq_lo_hz)     { cfg.freqLoHz      = *hdr.freq_lo_hz;     ++applied; }
    if (hdr.freq_hi_hz)     { cfg.freqHiHz      = *hdr.freq_hi_hz;     ++applied; }
    if (hdr.cricket_filter) { cfg.cricketFilter = *hdr.cricket_filter; ++applied; }
    return applied;
}

} // namespace echobox::replay
