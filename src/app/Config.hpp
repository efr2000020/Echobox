// SPDX-FileCopyrightText: 2026 The Echobox Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
/// @file
/// Single Config struct that holds every operator-tunable option for the
/// production binary. Populated by CliParser, validated by ConfigValidator,
/// then handed to Application which fans the fields out to each subsystem.

#include "logging/LogRecord.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace echobox::app {

/**
 * @brief Runtime configuration for the production binary.
 *
 * Every field has a sane default that matches a shipping field unit; the CLI
 * exposes a flag for each. Defaults that must stay in lock-step with another
 * module (e.g. @c snrThreshold tracking @c BandEnergyDetector's compiled
 * default) are called out in the per-field comments below.
 */
struct Config {
    // --- Audio capture ---
    std::string     device{"default"};
    int             sampleRate{384000};
    int             channels{1};

    // --- DSP ---
    std::string     algorithm{"BandEnergyDetector"};
    std::size_t     fftSize{4096};
    std::size_t     hopSize{512};
    int             freqLoHz{20000};
    int             freqHiHz{192000};   // Default suits the 384 kHz Ultramic
                                        // (=its Nyquist). For other mics, set
                                        // --sample-rate and --freq-hi-hz
                                        // together; ConfigValidator enforces
                                        // freqHiHz <= sampleRate / 2.
    // SNR threshold used by the active detector to decide a frame is "hot".
    // Plumbed through to the tracker as the `band_snr_threshold` tunable. The
    // default below must track BandEnergyDetector's compiled default so a user
    // who never passes --snr-threshold sees identical behavior to before this
    // flag existed; the two defaults are linked by ConfigValidator-style note,
    // not by build-time wiring, so update both together. DspPipelineConfig
    // carries a third copy of the same number — update all three.
    //
    // Lowered 12.0 -> 8.0 alongside max_flatness 0.65 -> 0.80: detector-level
    // per-call recall 85.9 / 90.8 % -> 97.4 / 97.9 % (session_01 /
    // session_02) at a detector duty cycle of ~14 % -> ~19 %. The duty rise
    // is the cost: more clips and more bytes per night on a solar-powered
    // Pi Zero 2 W. See BandEnergyDetector.hpp for the full sweep.
    float           snrThreshold{8.0f};

    // --- Recorder ---
    std::filesystem::path outputDir{"./recordings"};
    // Short-clip defaults (0.4.0 release): the customer runs BatDetect2
    // daily on a solar-powered, resource-limited device, so total audio
    // bytes per night is the currency that matters. The shipped 40 ms
    // end-to-end cap cuts MB/night by ~54 % against the previous 200 ms
    // cap while gaining +1.2 pp per-file presence recall and +5.7 pp
    // per-pass recall on the reference corpus (session 08-04-2026).
    // preRollMs covers the ~3 ms trigger latency with headroom;
    // silenceMs sits above the derived cricket-filter counter-race floor
    // (16 ms at 384 kHz / hop-512; see ConfigValidator). Operators who
    // want R1/R2v2-style grouping should pass the "long-pass profile"
    // documented in the README: --preroll-ms 1000 --silence-ms 2000
    // --max-length-ms 5000.
    std::uint32_t   preRollMs{10};
    std::uint32_t   silenceMs{20};
    // Min / max length of the saved WAV (pre-roll + active + hangover, end to
    // end). Recordings shorter than min are deleted instead of finalized;
    // recordings reaching max are closed early. maxLengthMs == 0 means "no
    // cap" so a user who only wants the min filter doesn't have to invent a
    // sentinel max. ConfigValidator enforces that maxLengthMs (when > 0)
    // leaves room for preRollMs + silenceMs, so the defaults below cannot
    // silently degenerate into "always closes before the call".
    std::uint32_t   minLengthMs{0};
    std::uint32_t   maxLengthMs{40};

    std::filesystem::path logDir{"./logs"};
    // Off by default: a shipped unit running unattended writes nothing to
    // disk unless the operator opts in. Application::run treats Off as
    // "don't even open a log file", so the SD card sees no I/O from the
    // logging subsystem. For field debugging pass --log-level info (or
    // debug). The console sink honours the same level.
    logging::LogLevel     logLevel{logging::LogLevel::Off};

    // Console sink (stderr, human-readable) mirrors the JSON-lines file
    // sink. Set to false to run headless without a terminal handler
    // attached.
    bool                  consoleLog{true};
    // Emit an "app: HEARTBEAT ..." record every this-many seconds so a
    // field operator can prove the unit is alive from a log tail. 0
    // disables.
    std::uint32_t         heartbeatSec{60};

    // Cricket filter master switch. One flag gates BOTH halves:
    //   * detector's noise_reject_enabled confident-reject gate
    //     (+ the temporal rep-guard, which is separately off by default);
    //   * recorder's cricketDiscard "no bat-like event" post-hoc drop.
    // On enables both; off disables both → the recorder behaves as if no
    // filter existed.
    //
    // Flipped back false -> true, now that the gate behind it is a
    // different gate. The old sweep-shape verdict kept only what it could
    // confirm was a bat, over four features measured non-discriminative
    // on this corpus (AUC 0.37-0.56) — it cost 63 pp of per-call recall.
    // The replacement rejects only what it can confirm is NOT a bat
    // (weak AND broadband AND low-band), over the three features that do
    // separate. Per-call recall against BatDetect2 proxy truth,
    // 120-file probe per session:
    //
    //                      session_01   session_02   non-bat removed
    //     filter OFF          98.38 %      99.06 %        0 %
    //     old sweep gate      37.49 %      35.87 %      ~78 %
    //     new rule (this)     96.04 %      97.66 %    41.4 % / 25.6 %
    //
    // The product target is >= 90 %; the new rule clears it on both
    // nights with ~6 pp of margin, for a 1-2 pp cost against filter-off.
    // That is a trade worth making — cricket/noise rejection is a
    // product requirement and unfiltered nights cost ~5x the clips.
    //
    // Scope of the claim: this is validated as a WEAK-BROADBAND-TRIGGER
    // filter. The non-bat population on these two nights is broadband
    // noise (median flatness 0.63) admitted by the snr 8 retune, not
    // tonal cricket harmonics, and neither night contains a
    // cricket-dominated stretch. A site with real cricket pressure can
    // tighten noise_snr_max / noise_flatness_min / noise_band_max at
    // runtime, or turn the whole thing off with --cricket-filter off.
    // See private_docs/audits/01_per_call_recall_audit.md §7b.
    bool                  cricketFilter{true};

    // --- Rejected-capture observability ---
    // Selects which cricket-discarded clips get preserved (into
    // <output>/rejected/) instead of being deleted. The feature is off by
    // default so a shipped unit is byte-identical to the pre-feature
    // recorder; when enabled it only adds writes to the parallel rejected/
    // sink, never mutates the accepted path.
    enum class SaveRejectedMode {
        Off,       ///< Feature disabled; no rejected/ dir is ever created.
        All,       ///< Save every rejected clip. Local / offline-validation mode.
        Sample,    ///< Save a random 1-in-N via saveRejectedSampleN.
        Boundary,  ///< Save only near-threshold ("plausible bat") rejects.
    };
    SaveRejectedMode      saveRejected{SaveRejectedMode::Off};
    /// 1-in-N sampling ratio used when saveRejected == Sample. Must be >= 1.
    std::uint32_t         saveRejectedSampleN{500};
    /// Storage governor: at most N rejected clips per rolling hour. Field
    /// deployments set a real cap (default 200); local runs pass 0 for
    /// unlimited when the "all" mode is used for offline validation.
    std::uint32_t         saveRejectedMaxPerHour{200};
};

} // namespace echobox::app
