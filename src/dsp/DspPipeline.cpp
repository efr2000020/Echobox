#include "DspPipeline.hpp"
#include "TrackerRegistry.hpp"
#include "logging/Logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

namespace litespec::dsp {

namespace {

std::uint64_t packLoHi(float lo, float hi) {
    std::uint32_t loBits = 0, hiBits = 0;
    std::memcpy(&loBits, &lo, sizeof(loBits));
    std::memcpy(&hiBits, &hi, sizeof(hiBits));
    return (static_cast<std::uint64_t>(hiBits) << 32)
         | static_cast<std::uint64_t>(loBits);
}

void unpackLoHi(std::uint64_t packed, float& lo, float& hi) {
    std::uint32_t loBits = static_cast<std::uint32_t>(packed & 0xFFFFFFFFu);
    std::uint32_t hiBits = static_cast<std::uint32_t>(packed >> 32);
    std::memcpy(&lo, &loBits, sizeof(lo));
    std::memcpy(&hi, &hiBits, sizeof(hi));
}

} // namespace

DspPipeline::DspPipeline(DspPipelineConfig cfg, LockFreeRingBuffer<float>& input)
    : m_cfg(std::move(cfg)), m_input(input) {}

DspPipeline::~DspPipeline() {
    stop();
}

void DspPipeline::start() {
    if (m_running.exchange(true, std::memory_order_acq_rel)) return;

    m_fft = makeFftEngine(m_cfg.hopSize, m_cfg.fftSize);

    const float hpfCutoff = std::max(20000.0f, m_cfg.freqLoHz);
    m_hpf.configureHighPass(hpfCutoff, static_cast<float>(m_cfg.sampleRate));

    auto& reg = ::TrackerRegistry::getInstance();
    m_tracker = reg.createTracker(m_cfg.algorithm);
    if (!m_tracker) {
        auto available = reg.getAvailableAlgorithms();
        if (!available.empty()) {
            LS_WARN("dsp", "algorithm '%s' not found; falling back to '%s'",
                    m_cfg.algorithm.c_str(), available[0].c_str());
            m_cfg.algorithm = available[0];
            m_tracker = reg.createTracker(m_cfg.algorithm);
        }
    }
    if (!m_tracker) {
        m_running.store(false, std::memory_order_release);
        throw std::runtime_error("dsp: no tracker plugins available");
    }

    m_tracker->configure(m_cfg.sampleRate, m_cfg.fftSize,
                         m_cfg.freqLoHz, m_cfg.freqHiHz);

    LS_INFO("dsp", "pipeline start algo=%s sr=%d fft=%zu hop=%zu hpf=%.0fHz",
            m_cfg.algorithm.c_str(), m_cfg.sampleRate,
            m_cfg.fftSize, m_cfg.hopSize, hpfCutoff);

    m_thread = std::thread(&DspPipeline::loop, this);
}

void DspPipeline::stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) return;
    if (m_thread.joinable()) m_thread.join();
    m_tracker.reset();
    m_fft.reset();
}

void DspPipeline::publish(bool active, float loHz, float hiHz) {
    const bool wasActive = m_active.load(std::memory_order_relaxed);
    m_loHiBits.store(packLoHi(loHz, hiHz), std::memory_order_release);
    m_active.store(active, std::memory_order_release);
    if (wasActive && !active) {
        m_generation.fetch_add(1, std::memory_order_release);
    }
}

litespec::recorder::DetectorStateSnapshot DspPipeline::snapshot() const {
    litespec::recorder::DetectorStateSnapshot s{};
    s.active     = m_active.load(std::memory_order_acquire);
    const auto p = m_loHiBits.load(std::memory_order_acquire);
    unpackLoHi(p, s.loHz, s.hiHz);
    s.generation = m_generation.load(std::memory_order_acquire);
    return s;
}

void DspPipeline::loop() {
    std::vector<float> timeBuffer(m_cfg.hopSize, 0.0f);
    std::vector<float> magBuffer(m_fft->getNumBins(), 0.0f);

    std::size_t   samplesCollected = 0;
    std::uint32_t currentFrame     = 0;
    float         sample           = 0.0f;

    while (m_running.load(std::memory_order_acquire)) {
        if (!m_input.pop(sample)) {
            std::this_thread::yield();
            continue;
        }
        sample = m_hpf.process(sample);
        timeBuffer[samplesCollected++] = sample;

        if (samplesCollected == m_cfg.hopSize) {
            m_fft->process(timeBuffer, magBuffer);

            DetectorState state{};
            Annotation    ann{};
            m_tracker->processFrame(magBuffer, currentFrame, state, ann);

            publish(state.active, state.lo_hz, state.hi_hz);

            samplesCollected = 0;
            ++currentFrame;
        }
    }
}

} // namespace litespec::dsp
