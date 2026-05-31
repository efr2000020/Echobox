#include "SignalHandler.hpp"

#include <csignal>
#include <cstring>

namespace echobox::common {

std::atomic<bool> SignalHandler::s_exitRequested{false};

namespace {
extern "C" void handler(int /*sig*/) {
    SignalHandler::requestExit();
}
} // namespace

void SignalHandler::install() {
    struct sigaction sa{};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Ignore SIGPIPE: the TCP dev sender otherwise dies on a vanished client.
    struct sigaction ign{};
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGPIPE, &ign, nullptr);
}

bool SignalHandler::shouldExit() {
    return s_exitRequested.load(std::memory_order_acquire);
}

void SignalHandler::requestExit() {
    s_exitRequested.store(true, std::memory_order_release);
}

} // namespace echobox::common
