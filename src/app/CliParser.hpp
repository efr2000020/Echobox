// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Hand-rolled command-line parser. Zero external deps (no Boost, no argp) so
/// the production binary stays small. Accepts both @c --key=value and
/// @c "--key value" forms.

#include "Config.hpp"
#include <string>

namespace echobox::app {

/**
 * @brief Outcome of one @c parseCli call.
 *
 * Distinguishes the four ways argv handling can end so @c main can dispatch
 * cleanly: continue with the populated Config (@c Ok), print help/version and
 * exit cleanly, or report a parse error and exit non-zero.
 */
struct CliResult {
    enum class Kind { Ok, HelpRequested, VersionRequested, Error };
    Kind        kind{Kind::Ok};
    /// Populated only when @c kind == @c Error. Human-readable, single line.
    std::string errorMessage;
};

/**
 * @brief Parse @c argv into the supplied @c Config.
 *
 * @param argc Argument count from @c main.
 * @param argv Argument vector from @c main.
 * @param cfg  Out-parameter: fields are overwritten for every flag seen;
 *             untouched fields keep their defaults from @c Config.
 * @return     @c Ok, @c HelpRequested, @c VersionRequested, or @c Error.
 *
 * @note Per-flag bounds (non-negative integers, recognised log levels, etc.)
 *       are enforced here. Cross-flag invariants live in @c ConfigValidator.
 */
CliResult parseCli(int argc, char** argv, Config& cfg);

/// Static help text, NUL-terminated. Caller prints it; never returns null.
const char* helpText();
/// Static version string, NUL-terminated. Caller prints it; never returns null.
const char* versionText();

} // namespace echobox::app
