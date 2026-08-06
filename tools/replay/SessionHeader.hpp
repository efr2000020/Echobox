// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Reader for the device's SESSION_HEADER.json (see src/collection/… for the
/// writer). Replay-only. The auditor's call: the tool cannot silently apply
/// one CLI config to a corpus recorded under a different one. When --input
/// points at (or into) a session directory, we lift the recording's actual
/// capture settings out of the header and use them as the defaults so CLI
/// flags are pure overrides on top of "how the device was really configured".
///
/// Deliberately not a general JSON parser: the file shape is fixed (we wrote
/// it) and dragging a JSON library into the shipping-adjacent link surface
/// would be strictly worse. Extracts flat scalars under the top-level
/// "config" object plus a couple of top-level fields; unrecognised keys are
/// ignored so a header written by a newer firmware still loads.

#include "app/Config.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace echobox::replay {

/// Effective config plumbed out of a session header. Any field the parser
/// couldn't find stays @c std::nullopt so the caller can distinguish "the
/// header didn't say" from "the header explicitly set it to the default".
struct SessionHeaderConfig {
    std::filesystem::path source;   ///< Path the header was read from.
    std::optional<int>           sample_rate;
    std::optional<int>           channels;
    std::optional<std::string>   algorithm;
    std::optional<std::uint32_t> preroll_ms;
    std::optional<std::uint32_t> silence_ms;
    std::optional<std::uint32_t> min_length_ms;
    std::optional<std::uint32_t> max_length_ms;
    std::optional<float>         snr_threshold;
    std::optional<std::size_t>   fft_size;
    std::optional<std::size_t>   hop_size;
    std::optional<int>           freq_lo_hz;
    std::optional<int>           freq_hi_hz;
    std::optional<bool>          cricket_filter;
};

/// Search @p inputPath and its parents for a SESSION_HEADER.json (up to
/// four levels — matches typical collection_YYYY-MM-DD/collection/ layouts
/// without walking to the filesystem root). Returns the path if found, or
/// empty if none was located.
std::filesystem::path findSessionHeader(const std::filesystem::path& inputPath);

/// Parse a SESSION_HEADER.json into a @c SessionHeaderConfig. Throws
/// @c std::runtime_error on IO / parse failure. Missing individual fields
/// are represented as @c std::nullopt, not an error.
SessionHeaderConfig readSessionHeader(const std::filesystem::path& jsonPath);

/// Apply the header's set fields onto @p cfg in-place. Only fields the
/// header actually carried are copied; anything else is left as the caller
/// set it. Returns the number of fields applied (for logging).
std::size_t applySessionHeader(const SessionHeaderConfig& hdr, echobox::app::Config& cfg);

} // namespace echobox::replay
