// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// Tests for the collection DecisionLog: append is atomic per line, records
/// survive process ordering, thread-safe under concurrent appenders.

#include <catch2/catch_test_macros.hpp>

#include "collection/DecisionLog.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using echobox::collection::DecisionLog;

namespace {

std::filesystem::path uniqueTmpFile(const char* tag) {
    const auto base = std::filesystem::temp_directory_path();
    const auto ns   = std::chrono::steady_clock::now().time_since_epoch().count();
    auto d = base / (std::string("echobox_decisionlog_test_") + tag + "_" + std::to_string(ns));
    std::filesystem::create_directories(d);
    return d / "decisions.jsonl";
}

std::vector<std::string> readLines(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::vector<std::string> out;
    std::string line;
    while (std::getline(in, line)) if (!line.empty()) out.push_back(line);
    return out;
}

} // namespace


TEST_CASE("DecisionLog: append writes one line per call, adds trailing newline",
          "[collection][decision-log]") {
    const auto path = uniqueTmpFile("append");
    {
        DecisionLog log(path);
        REQUIRE(log.append(R"({"kind":"event","start_sample":0})"));
        REQUIRE(log.append(R"({"kind":"decision","reason":"saved"})"));
    }
    const auto lines = readLines(path);
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0] == R"({"kind":"event","start_sample":0})");
    REQUIRE(lines[1] == R"({"kind":"decision","reason":"saved"})");
    std::filesystem::remove_all(path.parent_path());
}


TEST_CASE("DecisionLog: constructor overwrites existing file",
          "[collection][decision-log]") {
    const auto path = uniqueTmpFile("overwrite");
    { DecisionLog log(path); log.append("old-line"); }
    { DecisionLog log(path); log.append("new-line"); }
    const auto lines = readLines(path);
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0] == "new-line");
    std::filesystem::remove_all(path.parent_path());
}


TEST_CASE("DecisionLog: concurrent appends do not interleave lines",
          "[collection][decision-log]") {
    const auto path = uniqueTmpFile("concurrent");
    constexpr int kThreads       = 4;
    constexpr int kLinesPerThread = 100;
    {
        DecisionLog log(path);
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; ++t) {
            ts.emplace_back([&log, t] {
                for (int i = 0; i < kLinesPerThread; ++i) {
                    log.append("thread-" + std::to_string(t)
                             + "-line-" + std::to_string(i));
                }
            });
        }
        for (auto& th : ts) th.join();
    }
    const auto lines = readLines(path);
    REQUIRE(lines.size() == kThreads * kLinesPerThread);
    // Every line must match "thread-<t>-line-<i>" — no torn writes,
    // no interleaving of two threads' bytes on one line.
    for (const auto& L : lines) {
        REQUIRE(L.rfind("thread-", 0) == 0);
        auto dash = L.find("-line-");
        REQUIRE(dash != std::string::npos);
    }
    std::filesystem::remove_all(path.parent_path());
}
