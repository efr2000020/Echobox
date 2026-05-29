#include "app/Application.hpp"
#include "app/CliParser.hpp"

#include <cstdio>

int main(int argc, char** argv) {
    litespec::app::Config cfg;
    auto res = litespec::app::parseCli(argc, argv, cfg);
    switch (res.kind) {
        case litespec::app::CliResult::Kind::HelpRequested:
            std::fputs(litespec::app::helpText(), stdout);
            return 0;
        case litespec::app::CliResult::Kind::VersionRequested:
            std::fputs(litespec::app::versionText(), stdout);
            return 0;
        case litespec::app::CliResult::Kind::Error:
            std::fprintf(stderr, "error: %s\n\n", res.errorMessage.c_str());
            std::fputs(litespec::app::helpText(), stderr);
            return 2;
        case litespec::app::CliResult::Kind::Ok:
            break;
    }

    try {
        litespec::app::Application app(std::move(cfg));
        return app.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
