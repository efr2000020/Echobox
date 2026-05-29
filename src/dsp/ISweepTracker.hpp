#pragma once
#include <span>
#include <cstddef>
#include <cstdint>

// Stable, pre-existing detection event record. Emitted when a hot region closes.
#pragma pack(push, 1)
struct Annotation {
    uint32_t start_frame;
    uint32_t end_frame;
    float high_freq;
    float low_freq;
};
#pragma pack(pop)

// Per-frame activity snapshot. Lets downstream consumers (e.g. the recorder)
// react on the leading edge of a detection instead of waiting for end-of-event.
//
// `active` is true while the detector considers the current frame part of a
// (possibly open-ended) hot region. `lo_hz` / `hi_hz` describe the firing
// region's current freq span when active; their values are undefined when
// active is false.
struct DetectorState {
    bool  active;
    float lo_hz;
    float hi_hz;
};

class ISweepTracker {
public:
    virtual ~ISweepTracker() = default;

    /**
     * Called once per stream session, before any processFrame() call.
     *
     * @param sampleRate  Audio sample rate in Hz.
     * @param fftSize     FFT size in samples (so magnitudes.size() == fftSize/2 + 1).
     * @param freqLoHz    Lower edge of the detection search window in Hz.
     * @param freqHiHz    Upper edge of the detection search window in Hz.
     *                    Implementations should clamp to Nyquist internally.
     */
    virtual void configure(int sampleRate, std::size_t fftSize,
                           float freqLoHz, float freqHiHz) = 0;

    /**
     * Process a single FFT magnitude frame.
     *
     * @param magnitudes      fftSize/2 + 1 magnitudes for this hop.
     * @param currentFrame    Monotonic frame index since stream start.
     * @param outState        Always populated with the per-frame activity snapshot.
     * @param outAnnotation   Populated only when the return value is true.
     * @return true iff a complete detection event just closed; outAnnotation is valid.
     *
     * MUST be real-time safe: no malloc/new, no mutex, no blocking I/O.
     */
    virtual bool processFrame(std::span<const float> magnitudes,
                              std::uint32_t currentFrame,
                              DetectorState& outState,
                              Annotation& outAnnotation) = 0;
};

// --- Plugin entry points (resolved via dlsym by TrackerRegistry) ---
extern "C" {
    typedef ISweepTracker* (*CreateTrackerFunc)();
    typedef void           (*DestroyTrackerFunc)(ISweepTracker*);
    typedef const char*    (*GetTrackerNameFunc)();
}
