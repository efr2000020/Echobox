// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Session lifecycle for the data-collection overlay: pre-flight card
/// capacity report + session header, live governor loop, and a
/// SESSION_END marker on clean shutdown.
///
/// See DATA_COLLECTION_IMPL_VALIDATION_PLAN §2.3.

#include "CollectionConfig.hpp"
#include "SampleClock.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

namespace echobox::collection {

/**
 * @brief Static context stamped into the session header.
 *
 * Populated by @c Application from the runtime @c Config at start so the
 * offline analyst has a self-describing record of "what firmware and knobs
 * produced this run". Fields are copied by value at construction.
 */
struct SessionMetadata {
    std::string   firmwareSha;     ///< Compile-time git SHA (short).
    std::string   algorithm;       ///< Active detector plugin name.
    int           sampleRate{0};
    int           channels{0};
    std::string   micDevice;       ///< ALSA device string.
    std::string   siteNote;        ///< From CollectionConfig::siteNote.
    // Config knobs that changed the decision: pre-roll, silence, min/max
    // length, cricket-filter master switch. Written as a flat JSON blob so
    // adding a knob here does not require churn in every reader.
    std::string   configJsonBlob;
};

/**
 * @brief Session controller: header, governor, end-marker.
 *
 * Thread model: @c start() runs pre-flight synchronously (writes the header
 * file) then spawns one governor thread that polls @c SampleClock and
 * disk-free at @c governor.pollIntervalSec cadence. @c stop() joins the
 * governor thread and writes the @c SESSION_END marker. Idempotent.
 *
 * The governor does NOT close writer threads directly — it flips
 * @c stopRequested() which the application main loop watches, and the
 * application coordinates the same clean-shutdown path used on SIGINT so
 * there is exactly one shutdown code path (no "governor tore this down
 * while the recorder was mid-flush" race).
 */
class Session {
public:
    /**
     * @param cfg       Collection knobs (copied). Overlay is a no-op if
     *                  cfg.enabled == false — @c start() early-returns.
     * @param clock     Sample clock owned by the caller. Lifetime must
     *                  outlive this object.
     * @param outputDir Where the session header, governor state file, and
     *                  SESSION_END marker are written. Usually
     *                  @c cfg.dir; broken out so tests can inject a tmp
     *                  path without mutating cfg.
     */
    Session(CollectionConfig cfg, const SampleClock& clock,
            std::filesystem::path outputDir);
    ~Session();

    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    /**
     * @brief Pre-flight + governor thread spawn.
     *
     * Reads the free capacity of @c outputDir, computes the maximum
     * recordable duration at the caller's audio settings, writes
     * @c SESSION_HEADER.json (containing @p meta + capacity + wall/sample
     * anchor), then spawns the governor. No-op when @c cfg.enabled == false.
     *
     * @return true on success. false on pre-flight failure (dir not
     *         creatable, capacity read failed) — the caller should log and
     *         either continue without collection or abort per policy.
     */
    bool start(const SessionMetadata& meta);

    /**
     * @brief Join the governor thread and write the SESSION_END marker.
     * Idempotent; safe to call from the shutdown handler even if @c start()
     * returned false.
     */
    void stop();

    /**
     * @brief True once the governor has decided the run must stop cleanly
     *        (max duration reached, or free-space floor hit).
     *
     * The application's main loop should check this alongside its existing
     * SignalHandler poll so the shutdown code path is the same on
     * governor-trip as it is on SIGINT.
     */
    bool stopRequested() const {
        return m_stopRequested.load(std::memory_order_acquire);
    }

    /**
     * @brief Human-readable reason for the last stop-request, or empty
     *        string if the run is still going. Written to the SESSION_END
     *        marker.
     */
    std::string stopReason() const;

    // Accessors used by unit tests and status logging.
    const CollectionConfig& config() const { return m_cfg; }
    std::filesystem::path   headerPath() const;
    std::filesystem::path   endMarkerPath() const;

private:
    void governorLoop();
    bool writeHeader(const SessionMetadata& meta,
                     std::uint64_t freeMbAtStart,
                     double hoursAtStart) const;
    bool writeEndMarker() const;

    CollectionConfig       m_cfg;
    const SampleClock&     m_clock;
    std::filesystem::path  m_outputDir;

    std::atomic<bool>      m_started{false};
    std::atomic<bool>      m_running{false};
    std::atomic<bool>      m_stopRequested{false};
    std::atomic<bool>      m_endWritten{false};
    std::thread            m_governor;

    // Populated when stopRequested() flips true. Read once at stop() to
    // stamp the end marker. Protected by m_stopRequested's release/acquire
    // pair (only ever written before the flag flips).
    std::string            m_stopReason;

    // Anchors for the header. Set in start().
    std::chrono::system_clock::time_point m_startWall{};
    std::uint64_t                         m_startSample{0};
};

/**
 * @brief Estimate hours of continuous mono-int16 audio that fits in
 *        @p freeMb megabytes at @p sampleRate.
 *
 * Extracted for unit-testability and to keep the header-writer honest
 * about which storage math it's quoting.
 */
double estimateMaxHours(std::uint64_t freeMb, int sampleRate);

/**
 * @brief Free megabytes on the filesystem containing @p path. Returns
 *        0 (and logs) on error.
 */
std::uint64_t availableMb(const std::filesystem::path& path);

} // namespace echobox::collection
