#pragma once
#include <cstdint>

namespace litespec::recorder {

struct DetectorStateSnapshot {
    bool          active;
    float         loHz;       // valid only while active
    float         hiHz;       // valid only while active
    std::uint64_t generation; // increments on every active→inactive transition
};

/**
 * Read-only view of the DSP pipeline's current detector state, polled by the
 * Recorder thread. Implementations must be wait-free; the Recorder calls
 * snapshot() at a few-millisecond cadence.
 */
class IDetectorStateProvider {
public:
    virtual ~IDetectorStateProvider() = default;
    virtual DetectorStateSnapshot snapshot() const = 0;
};

} // namespace litespec::recorder
