#pragma once
#include "DetectorStateProvider.hpp"
#include "FilenameBuilder.hpp"
#include "PreRollBuffer.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>

namespace litespec::recorder {

class WavWriter;

struct RecorderConfig {
    std::filesystem::path outputDir{"./recordings"};
    int                   sampleRate{384000};
    int                   channels{1};
    std::uint32_t         preRollMs{1000};
    std::uint32_t         silenceMs{100};
    /** How often the recorder polls the detector state. */
    std::uint32_t         pollIntervalMs{5};
};

/**
 * Recorder gate.
 *
 * Subscribes to a DetectorStateProvider and a PreRollBuffer fed continuously
 * by the audio thread. On a detection rising edge: opens a temp WAV, dumps
 * the configured pre-roll, then streams live samples until the detector has
 * been quiet for `silenceMs`. On close, renames the file to its canonical
 * name (timestamp + duration + firing-band span).
 */
class Recorder {
public:
    Recorder(RecorderConfig cfg,
             const PreRollBuffer& preRoll,
             const IDetectorStateProvider& detector);
    ~Recorder();

    Recorder(const Recorder&)            = delete;
    Recorder& operator=(const Recorder&) = delete;

    void start();
    void stop();

private:
    enum class State { Idle, Active };

    void loop();

    void beginRecording(const DetectorStateSnapshot& s);
    void appendLiveAudio();
    void endRecording();

    RecorderConfig                m_cfg;
    const PreRollBuffer&          m_preRoll;
    const IDetectorStateProvider& m_detector;

    FilenameBuilder               m_names;
    std::atomic<bool>             m_running{false};
    std::thread                   m_thread;

    // --- active recording state ---
    State                                m_state{State::Idle};
    std::unique_ptr<WavWriter>           m_writer;
    PreRollBuffer::Cursor                m_cursor;
    std::chrono::steady_clock::time_point m_eventStartSteady;
    std::chrono::system_clock::time_point m_eventStartWall;
    std::chrono::steady_clock::time_point m_lastActiveTime;
    float                                 m_eventLoHz{0.0f};
    float                                 m_eventHiHz{0.0f};
    std::filesystem::path                 m_currentTempPath;
};

} // namespace litespec::recorder
