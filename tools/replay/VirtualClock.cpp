// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file
/// VirtualClock implementation. See VirtualClock.hpp for the contract and
/// for why replay needs it at all.

#include "VirtualClock.hpp"

#include <thread>

namespace echobox::replay {

VirtualClock::VirtualClock(std::chrono::system_clock::time_point wallEpoch)
    : m_wallEpoch(wallEpoch) {}

std::chrono::steady_clock::time_point VirtualClock::now() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(
        static_cast<std::chrono::nanoseconds::rep>(m_ns)));
}

std::chrono::system_clock::time_point VirtualClock::wallNow() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_wallEpoch + std::chrono::nanoseconds(
        static_cast<std::chrono::nanoseconds::rep>(m_ns));
}

std::uint64_t VirtualClock::nowNs() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_ns;
}

void VirtualClock::sleepFor(std::chrono::milliseconds d) {
    std::unique_lock<std::mutex> lk(m_mutex);
    if (m_released) {
        // Shutdown path: the feeder has stopped advancing time, so waiting
        // on it would hang the join in Recorder::stop(). Fall back to a real
        // sleep, which keeps the recorder's poll cadence sane for the handful
        // of iterations between release() and the running flag going false.
        lk.unlock();
        std::this_thread::sleep_for(d);
        return;
    }

    auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
    // A zero-length poll interval would make the barrier's "parked for a
    // future time" test trivially false and stall the feeder forever. The
    // recorder would spin on a real device too, so clamping to one tick is
    // the faithful reading, not a behaviour change.
    if (ns == 0) ns = 1;

    m_wakeNs = m_ns + ns;
    m_parked = true;
    // Tell the feeder the poll it woke us for is complete. Notify while
    // holding the lock: the feeder's predicate reads m_parked and m_wakeNs
    // together, and they must be seen as one update.
    m_cvFeeder.notify_all();

    m_cvRecorder.wait(lk, [this] { return m_released || m_ns >= m_wakeNs; });
    m_parked = false;
}

void VirtualClock::waitUntilRecorderIdle() {
    std::unique_lock<std::mutex> lk(m_mutex);
    m_cvFeeder.wait(lk, [this] {
        return m_released || (m_parked && m_wakeNs > m_ns);
    });
}

void VirtualClock::advanceTo(std::chrono::nanoseconds virtualNow) {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const auto ns = static_cast<std::uint64_t>(
            virtualNow.count() > 0 ? virtualNow.count() : 0);
        // Monotonic by construction (the driver's tick index only grows),
        // but clamp rather than trust every future caller: a clock that
        // went backwards would quietly corrupt the silence timeout instead
        // of failing somewhere visible.
        if (ns > m_ns) m_ns = ns;
    }
    m_cvRecorder.notify_all();
}

void VirtualClock::release() {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_released = true;
    }
    m_cvRecorder.notify_all();
    m_cvFeeder.notify_all();
}

} // namespace echobox::replay
