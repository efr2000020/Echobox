#pragma once
#include "Config.hpp"
#include <string>

namespace echobox::app {

struct CliResult {
    enum class Kind { Ok, HelpRequested, VersionRequested, Error };
    Kind        kind{Kind::Ok};
    std::string errorMessage;
};

/**
 * Parses argv into the supplied Config. Returns Ok on success, or one of the
 * non-Ok kinds for --help / --version / parse errors. On HelpRequested or
 * VersionRequested the caller is expected to print the relevant text and exit.
 *
 * Hand-rolled to keep the dependency footprint at zero for the production binary.
 * Accepts both `--key value` and `--key=value`.
 */
CliResult parseCli(int argc, char** argv, Config& cfg);

/** Returns the static help text. Caller prints it. */
const char* helpText();
const char* versionText();

} // namespace echobox::app
