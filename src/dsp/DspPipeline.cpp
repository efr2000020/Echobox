// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// DspPipeline implementation. See DspPipeline.hpp for the public contract.

#include "DspPipeline.hpp"
#include "TrackerRegistry.hpp"
#include "logging/Logger.hpp"
#include "recorder/Sidecar.hpp"   // TunableValue

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

namespace echobox::dsp {

namespace {

// Pack/unpack two floats into one uint64 so the recorder can read both with
// a single atomic load (see m_loHiBits in the header).

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

    // Apply the user's SNR threshold. The tunable key is BandEnergyDetector's,
    // but it's the canonical name a plugin author would pick for the same
    // concept. If a future plugin doesn't expose it, fall through with a
    // warning rather than refusing to start — losing a single knob shouldn't
    // take a field unit offline overnight.
    if (!m_tracker->setTunable("band_snr_threshold",
                               static_cast<double>(m_cfg.snrThreshold))) {
        LS_WARN("dsp", "tracker '%s' does not accept band_snr_threshold; "
                       "--snr-threshold ignored",
                m_cfg.algorithm.c_str());
    }

    // Cricket filter: flip the tracker's confident-reject noise gate to
    // match the operator's --cricket-filter setting. Trackers that don't
    // ship the knob log a debug notice and continue — that's fine, the
    // recorder's cricketDiscard half will end up as a no-op too (no
    // bat-like counter ever increments to compare against, but no
    // discard-on-empty either, because a plain tracker's
    // @c batLikeEventsSinceBoot default = totalEventsSinceBoot() ie.
    // always changes when an event closes).
    if (!m_tracker->setTunable("noise_reject_enabled",
                               m_cfg.cricketFilter ? 1.0 : 0.0)) {
        LS_DEBUG("dsp", "tracker '%s' does not accept noise_reject_enabled; "
                        "--cricket-filter has no detector-side effect",
                 m_cfg.algorithm.c_str());
    }

    // Arm the tracker's collection-only events queue, if the overlay
    // asked for it. Done here rather than from the collection poller
    // because the tracker is created three lines up — the poller is
    // constructed and started before this point, so arming from there
    // would no-op against a null tracker. Draining an empty queue is
    // exactly what arms it; see ISweepTracker::drainCollectionEvents.
    if (m_cfg.collectEvents) {
        std::vector<EventFeatures> arming;
        if (!m_tracker->drainCollectionEvents(arming)) {
            LS_WARN("dsp", "tracker '%s' does not implement "
                           "drainCollectionEvents; collection Stream C will "
                           "record no per-event data this session",
                    m_cfg.algorithm.c_str());
        }
    }

    // The temporal repetition-rate guard needs the hop size to convert
    // onset frame indices into seconds. Pipeline is the authoritative
    // source (configure() takes sampleRate + fftSize but not hop). Older
    // trackers ignore the knob silently.
    m_tracker->setTunable("hop_size_samples",
                          static_cast<double>(m_cfg.hopSize));

    // Diagnostic overrides applied here — before the worker thread is
    // spawned — so there is no window where a frame could be processed
    // with the built-in default instead of the override. Shipping
    // Application leaves this empty; workstation echobox-replay uses it
    // for --tunable KEY=VALUE forwarding.
    for (const auto& [key, value] : m_cfg.extraTrackerTunables) {
        if (!m_tracker->setTunable(key.c_str(), value)) {
            LS_WARN("dsp", "tracker '%s' rejected --tunable %s=%g",
                    m_cfg.algorithm.c_str(), key.c_str(), value);
        } else {
            LS_INFO("dsp", "applied --tunable %s=%g", key.c_str(), value);
        }
    }

    LS_INFO("dsp", "pipeline start algo=%s sr=%d fft=%zu hop=%zu hpf=%.0fHz snr=%.2f",
            m_cfg.algorithm.c_str(), m_cfg.sampleRate,
            m_cfg.fftSize, m_cfg.hopSize, hpfCutoff, m_cfg.snrThreshold);

    m_thread = std::thread(&DspPipeline::loop, this);
}

bool DspPipeline::setTrackerTunable(const char* key, double value) {
    // Diagnostic forwarder — see header. If the tracker plugin isn't
    // loaded (e.g. called before start() or after stop()), we can't
    // apply the tunable; return false so a workstation caller can
    // log-and-continue rather than silently proceed with the wrong
    // gate setting.
    if (!m_tracker) return false;
    return m_tracker->setTunable(key, value);
}

void DspPipeline::stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) return;
    // Wake the DSP thread in case it's parked on the input CV — otherwise
    // shutdown stalls for up to the wait_for timeout.
    notifyInput();
    if (m_thread.joinable()) m_thread.join();
    m_tracker.reset();
    m_fft.reset();
}

void DspPipeline::notifyInput() {
    {
        std::lock_guard<std::mutex> lk(m_wakeMutex);
    }
    m_wakeCv.notify_one();
}

void DspPipeline::publish(bool active, float loHz, float hiHz,
                          std::uint64_t batLikeEvents) {
    const bool wasActive = m_active.load(std::memory_order_relaxed);
    m_loHiBits.store(packLoHi(loHz, hiHz), std::memory_order_release);
    // Publish batLikeEvents BEFORE flipping active low. The recorder reads
    // the counter at endRecording() time, which is triggered by observing
    // active=false — release-order here means the reader that sees the
    // active flip also sees the counter increment from a just-closed event.
    m_batLikeEvents.store(batLikeEvents, std::memory_order_release);
    m_active.store(active, std::memory_order_release);
    if (wasActive && !active) {
        m_generation.fetch_add(1, std::memory_order_release);
    }
}

echobox::recorder::DetectorStateSnapshot DspPipeline::snapshot() const {
    echobox::recorder::DetectorStateSnapshot s{};
    s.active     = m_active.load(std::memory_order_acquire);
    const auto p = m_loHiBits.load(std::memory_order_acquire);
    unpackLoHi(p, s.loHz, s.hiHz);
    s.generation    = m_generation.load(std::memory_order_acquire);
    s.batLikeEvents = m_batLikeEvents.load(std::memory_order_acquire);
    return s;
}

bool DspPipeline::drainSidecarPayload(SidecarPayload& out) {
    if (!m_tracker) return false;
    return m_tracker->drainSidecarPayload(out);
}

bool DspPipeline::drainCollectionEvents(std::vector<EventFeatures>& out) {
    if (!m_tracker) return false;
    return m_tracker->drainCollectionEvents(out);
}

bool DspPipeline::readNoiseFloor(std::vector<float>& out) const {
    if (!m_tracker) return false;
    return m_tracker->readNoiseFloor(out);
}

bool DspPipeline::currentTunables(
        std::vector<echobox::recorder::TunableValue>& out) const {
    if (!m_tracker) return false;
    out.clear();
    const auto manifest = m_tracker->listTunables();
    out.reserve(manifest.size());
    for (const auto& t : manifest) {
        if (!t.key) continue;
        double v = 0.0;
        if (!m_tracker->getTunable(t.key, &v)) continue;
        echobox::recorder::TunableValue tv;
        tv.key    = t.key;
        tv.value  = v;
        tv.is_int = (t.type == TunableType::Int);
        out.push_back(std::move(tv));
    }
    return true;
}

std::string DspPipeline::algorithmName() const {
    return m_cfg.algorithm;
}

void DspPipeline::loop() {
    std::vector<float> timeBuffer(m_cfg.hopSize, 0.0f);
    std::vector<float> magBuffer(m_fft->getNumBins(), 0.0f);

    std::size_t   samplesCollected = 0;
    std::uint32_t currentFrame     = 0;
    float         sample           = 0.0f;

    while (m_running.load(std::memory_order_acquire)) {
        if (!m_input.pop(sample)) {
            // Ring empty — park until the producer notifies. ALSA delivers
            // ~10 ms bursts, so without this we'd burn CPU for the whole
            // inter-burst gap. The short timeout bounds shutdown latency in
            // case a notify is missed between the pop check and the wait.
            std::unique_lock<std::mutex> lk(m_wakeMutex);
            m_wakeCv.wait_for(lk, std::chrono::milliseconds(50), [this] {
                return !m_running.load(std::memory_order_acquire)
                    || m_input.available_read() > 0;
            });
            continue;
        }
        sample = m_hpf.process(sample);
        timeBuffer[samplesCollected++] = sample;

        if (samplesCollected == m_cfg.hopSize) {
            m_fft->process(timeBuffer, magBuffer);

            DetectorState state{};
            Annotation    ann{};
            m_tracker->processFrame(magBuffer, currentFrame, state, ann);

            publish(state.active, state.lo_hz, state.hi_hz,
                    m_tracker->batLikeEventsSinceBoot());

            samplesCollected = 0;
            ++currentFrame;
        }
    }
}

} // namespace echobox::dsp
