#pragma once
#include "Config.hpp"

#include <string>
#include <vector>

namespace echobox::app {

/**
 * Cross-flag config sanity checks.
 *
 * CLI argument parsing in CliParser already enforces *per-flag* bounds
 * (non-negative integers, recognised log levels, etc.). This module handles
 * *combinations* of flags that look fine individually but break in concert
 * — e.g. a max-length-ms shorter than (pre-roll + silence) would close every
 * recording before the detector could even publish a leading edge.
 *
 * `validateConfig` returns an empty vector when every invariant holds;
 * otherwise one human-readable error string per violated invariant. All
 * checks run unconditionally so the operator sees every problem at once
 * and can fix them in a single edit cycle rather than N relaunches.
 *
 * Extending: write a new `check_X(const Config&)` helper in the .cpp file
 * returning `std::optional<std::string>`, then add one line to
 * `validateConfig` to call it. No registration ceremony, no plugin layer —
 * just one new helper per new invariant.
 */
std::vector<std::string> validateConfig(const Config& cfg);

} // namespace echobox::app
