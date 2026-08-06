// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Run-level provenance manifest. Written to @c <output>/replay_manifest.json
/// once per invocation so any downstream scorer reading the accepted/ +
/// rejected/ tree can pin exactly what generated it — critically, that this
/// was an x86 replay (not a device run), plus the build type, compiler
/// flags that affect numerics (-ffast-math), effective capture config, and
/// where that config came from (session header vs CLI defaults).
///
/// Sidecars already carry the per-clip recorder/detector config accurately
/// (the shipping Recorder writes them). What sidecars can't say — because
/// they're written by shipping code that has no concept of "replay" — is
/// "this was an x86 replay of a recorded file, not a live device capture".
/// The manifest fills that specific gap.

#include "app/Config.hpp"
#include "SessionHeader.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace echobox::replay {

struct RunManifest {
    std::string              tool_version;      ///< echobox-replay version tag.
    std::string              platform;          ///< e.g. "x86-replay".
    std::string              build_type;        ///< CMAKE_BUILD_TYPE at compile.
    bool                     fast_math{false};  ///< True iff -ffast-math was on.
    std::string              compiler;          ///< __VERSION__ if available.
    std::filesystem::path    input;
    std::filesystem::path    output;
    std::string              config_source;     ///< "session_header:<path>" | "cli_defaults".
    std::vector<std::string> cli_argv;          ///< As given on the command line.
    std::string              run_iso8601;       ///< Wall-clock start of run.
    echobox::app::Config     config;            ///< Effective config after CLI overrides.
    bool                     had_session_header{false};
    SessionHeaderConfig      session_header;    ///< As-loaded, when present.
};

/// Serialise @p m to @p path (creates parent dirs). Dependency-free JSON
/// writer — same discipline as recorder/Sidecar.cpp: no nlohmann/json in
/// the link surface of code adjacent to shipping libraries.
void writeRunManifest(const std::filesystem::path& path, const RunManifest& m);

} // namespace echobox::replay
