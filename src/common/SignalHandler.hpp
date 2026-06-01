#pragma once
/// @file
/// Process-wide shutdown signal. Application polls @c shouldExit() from its
/// main loop; subsystems may call @c requestExit() on a fatal error.

#include <atomic>

namespace echobox::common {

/**
 * @brief Process-wide shutdown flag, set by SIGINT/SIGTERM.
 *
 * @c install() wires the signal handlers and ignores SIGPIPE. @c shouldExit()
 * is the polling primitive the main loop watches; @c requestExit() is the
 * same flag, callable from in-process error paths so a subsystem can shut
 * the binary down without raising a signal.
 *
 * @note Header-only state lives in a single static atomic; safe to call any
 *       of these from any thread (including a signal handler).
 */
class SignalHandler {
public:
    /// Install SIGINT/SIGTERM handlers and ignore SIGPIPE. Idempotent.
    static void install();
    /// @return @c true once a shutdown signal has been raised or requested.
    static bool shouldExit();
    /// In-process equivalent of receiving SIGINT/SIGTERM (e.g. fatal error).
    static void requestExit();

private:
    static std::atomic<bool> s_exitRequested;
};

} // namespace echobox::common
