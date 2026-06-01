/// @file
/// Production binary entry point. Parses CLI, validates the config, hands
/// the result to Application::run() and forwards its exit code.

#include "app/Application.hpp"
#include "app/CliParser.hpp"
#include "app/ConfigValidator.hpp"

#include <cstdio>

int main(int argc, char** argv) {
    echobox::app::Config cfg;
    auto res = echobox::app::parseCli(argc, argv, cfg);
    switch (res.kind) {
        case echobox::app::CliResult::Kind::HelpRequested:
            std::fputs(echobox::app::helpText(), stdout);
            return 0;
        case echobox::app::CliResult::Kind::VersionRequested:
            std::fputs(echobox::app::versionText(), stdout);
            return 0;
        case echobox::app::CliResult::Kind::Error:
            std::fprintf(stderr, "error: %s\n\n", res.errorMessage.c_str());
            std::fputs(echobox::app::helpText(), stderr);
            return 2;
        case echobox::app::CliResult::Kind::Ok:
            break;
    }

    // Catch cross-flag combinations that look fine individually but break
    // in concert. Report every violation in one go so the user fixes all
    // of them in a single edit rather than one launch per error.
    const auto configErrors = echobox::app::validateConfig(cfg);
    if (!configErrors.empty()) {
        for (const auto& msg : configErrors) {
            std::fprintf(stderr, "error: %s\n", msg.c_str());
        }
        return 2;
    }

    try {
        echobox::app::Application app(std::move(cfg));
        return app.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
