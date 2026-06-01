// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Cross-flag config sanity checks. @c CliParser enforces per-flag bounds;
/// this module enforces invariants that span flags (e.g. max-length ≥
/// pre-roll + silence).

#include "Config.hpp"

#include <string>
#include <vector>

namespace echobox::app {

/**
 * @brief Validate cross-flag invariants on a populated @c Config.
 *
 * @param cfg Configuration to check; not mutated.
 * @return Empty vector when every invariant holds, otherwise one
 *         human-readable error string per violation.
 *
 * Every check runs unconditionally so the operator sees the full list in
 * one launch instead of fixing one error at a time.
 *
 * @note Adding a new invariant: write a new @c checkX(const Config&) helper
 *       in the @c .cpp returning @c std::optional<std::string>, then add one
 *       @c run() line to @c validateConfig. No registration ceremony.
 */
std::vector<std::string> validateConfig(const Config& cfg);

} // namespace echobox::app
