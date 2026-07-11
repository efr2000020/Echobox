// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// Sidecar JSON emitter. See Sidecar.hpp for the public contract.
///
/// Implementation notes:
///   - Hand-rolled JSON writer: the only structured payload is shallow
///     (a few objects + an array of events), so a 50-line writer is
///     simpler than vendoring nlohmann/json into the production link.
///   - Atomic write: builds the JSON in a sibling temp file, then renames
///     it into place — if the device dies mid-write, the consumer never
///     sees a half-written sidecar.

#include "Sidecar.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <system_error>

namespace echobox::recorder {

namespace {

constexpr const char kB64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Standard MIME base64; LSB-first byte order matches float32 native layout
// on every platform we target (ARMv8, x86_64) — no endianness conversion.
std::string base64Encode(const std::uint8_t* data, std::size_t n) {
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    for (std::size_t i = 0; i < n; i += 3) {
        const std::uint32_t b0 = data[i];
        const std::uint32_t b1 = (i + 1 < n) ? data[i + 1] : 0;
        const std::uint32_t b2 = (i + 2 < n) ? data[i + 2] : 0;
        const std::uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kB64Chars[(triple >> 18) & 0x3F]);
        out.push_back(kB64Chars[(triple >> 12) & 0x3F]);
        out.push_back((i + 1 < n) ? kB64Chars[(triple >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < n) ? kB64Chars[ triple        & 0x3F] : '=');
    }
    return out;
}

std::vector<std::uint8_t> base64Decode(const std::string& b64) {
    // Reverse map; sentinel 0xFF for non-base64 characters.
    static const auto kRev = [] {
        std::array<std::uint8_t, 256> r{};
        r.fill(0xFF);
        for (std::uint8_t i = 0; i < 64; ++i) {
            r[static_cast<unsigned char>(kB64Chars[i])] = i;
        }
        return r;
    }();
    std::vector<std::uint8_t> out;
    out.reserve((b64.size() / 4) * 3);
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : b64) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const std::uint8_t v = kRev[static_cast<unsigned char>(c)];
        if (v == 0xFF) return {};  // garbage in → empty out
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

// Escape a UTF-8 string for JSON. We only ever pass ASCII into this (paths,
// algorithm names, ISO-8601 timestamps), so the minimal set is enough.
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
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
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// 6 significant digits is enough for the diagnostic values we emit (SNRs,
// flatness, kHz freqs) and keeps sidecar size bounded. Avoid the C++17 stream
// locale dependency for "%g"-style formatting; printf is fine and matches
// what LS_INFO produces.
std::string fmtFloat(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

} // namespace


std::string base64EncodeFloats(const std::vector<float>& floats) {
    if (floats.empty()) return {};
    return base64Encode(reinterpret_cast<const std::uint8_t*>(floats.data()),
                        floats.size() * sizeof(float));
}

std::vector<float> base64DecodeFloats(const std::string& b64) {
    auto bytes = base64Decode(b64);
    if (bytes.size() % sizeof(float) != 0) return {};
    std::vector<float> out(bytes.size() / sizeof(float));
    if (!out.empty()) std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}


bool writeSidecar(const std::filesystem::path& wavPath,
                  const SidecarRecording& meta,
                  const std::vector<TunableValue>& tunables,
                  const SidecarPayload& payload) {
    std::filesystem::path finalPath = wavPath;
    finalPath.replace_extension(".json");
    std::filesystem::path tempPath = finalPath;
    tempPath += ".tmp";

    std::ostringstream os;
    os.imbue(std::locale::classic());

    os << "{\n";
    os << "  \"format_version\": 1,\n";

    // --- device counters ---
    os << "  \"device\": {\n";
    os << "    \"boot_ts\": \""              << jsonEscape(meta.boot_iso8601) << "\",\n";
    os << "    \"events_total_since_boot\": " << payload.events_total_since_boot     << ",\n";
    os << "    \"frames_processed\": "        << payload.frames_processed_since_boot << "\n";
    os << "  },\n";

    // --- recording metadata ---
    os << "  \"recording\": {\n";
    os << "    \"wav_path\": \""        << jsonEscape(meta.wav_path)        << "\",\n";
    os << "    \"capture_ts\": \""      << jsonEscape(meta.capture_iso8601) << "\",\n";
    os << "    \"sample_rate\": "       << meta.sample_rate                 << ",\n";
    os << "    \"fft_size\": "          << meta.fft_size                    << ",\n";
    os << "    \"hop_size\": "          << meta.hop_size                    << ",\n";
    os << "    \"freq_lo_hz\": "        << fmtFloat(meta.freq_lo_hz)        << ",\n";
    os << "    \"freq_hi_hz\": "        << fmtFloat(meta.freq_hi_hz)        << ",\n";
    os << "    \"preroll_ms\": "        << meta.preroll_ms                  << ",\n";
    os << "    \"silence_ms\": "        << meta.silence_ms                  << "\n";
    os << "  },\n";

    // --- detector + tunables + floor snapshot ---
    os << "  \"detector\": {\n";
    os << "    \"algorithm\": \"" << jsonEscape(meta.algorithm) << "\",\n";
    os << "    \"tunables\": {";
    for (std::size_t i = 0; i < tunables.size(); ++i) {
        const auto& t = tunables[i];
        if (i > 0) os << ",";
        os << "\n      \"" << jsonEscape(t.key) << "\": ";
        if (t.is_int) {
            os << static_cast<long long>(t.value);
        } else {
            os << fmtFloat(t.value);
        }
    }
    os << (tunables.empty() ? "" : "\n    ") << "},\n";

    os << "    \"noise_floor_at_first_event\": {\n";
    os << "      \"n_bins\": "    << payload.noise_floor_at_first_event.size() << ",\n";
    os << "      \"encoding\": \"float32_base64\",\n";
    os << "      \"data\": \""
       << base64EncodeFloats(payload.noise_floor_at_first_event) << "\"\n";
    os << "    }\n";
    os << "  },\n";

    // --- events ---
    os << "  \"events\": [";
    for (std::size_t i = 0; i < payload.events.size(); ++i) {
        const auto& e = payload.events[i];
        if (i > 0) os << ",";
        os << "\n    {";
        os << "\"start_frame\": "      << e.start_frame
           << ", \"end_frame\": "      << e.end_frame
           << ", \"duration_frames\": "<< e.duration_frames
           << ", \"band_index\": "     << static_cast<int>(e.band_index)
           << ", \"trigger_snr\": "    << fmtFloat(e.trigger_snr)
           << ", \"trigger_flatness\": "<< fmtFloat(e.trigger_flatness)
           << ", \"peak_snr\": "       << fmtFloat(e.peak_snr)
           << ", \"lo_hz\": "          << fmtFloat(e.lo_hz)
           << ", \"hi_hz\": "          << fmtFloat(e.hi_hz)
           << ", \"bandwidth_khz\": "  << fmtFloat(e.bandwidth_khz)
           << ", \"drift_khz\": "      << fmtFloat(e.drift_khz)
           << ", \"path_ratio\": "     << fmtFloat(e.path_ratio)
           << ", \"mono_fraction\": "  << fmtFloat(e.mono_fraction)
           << ", \"gate_rejected\": "  << (e.gate_rejected ? "true" : "false")
           << "}";
    }
    os << (payload.events.empty() ? "" : "\n  ") << "]\n";
    os << "}\n";

    // Atomic write: temp + rename. If we crash mid-write the consumer never
    // sees a half-formed sidecar — the canonical .json simply isn't there.
    {
        std::ofstream f(tempPath, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        const std::string s = os.str();
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tempPath, finalPath, ec);
    if (ec) {
        std::filesystem::remove(tempPath, ec);
        return false;
    }
    return true;
}

} // namespace echobox::recorder
