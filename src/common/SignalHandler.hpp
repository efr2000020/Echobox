#pragma once
#include <atomic>

namespace echobox::common {

/**
 * Installs SIGINT / SIGTERM handlers that set a process-wide atomic flag.
 * The Application polls this flag to start a clean shutdown.
 */
class SignalHandler {
public:
    static void install();
    static bool shouldExit();
    static void requestExit();   // for in-process callers (e.g. error paths)

private:
    static std::atomic<bool> s_exitRequested;
};

} // namespace echobox::common
