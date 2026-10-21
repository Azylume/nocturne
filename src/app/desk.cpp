#include "desk/desk_server.hpp"

#include <pthread.h>
#include <signal.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr const char* kUsage = R"(usage: nocturne-desk [--port N] [--engine-port N] [--loopback-only]
Serves the venue desk to phones on the venue LAN, from the engine on this machine.
  --port N          HTTP port (default 7480; 0 picks a free one)
  --engine-port N   the engine's protocol port (default 7400)
  --loopback-only   listen on 127.0.0.1 alone and answer it (tests)
)";

[[noreturn]] void usage(int code)
{
    std::fputs(kUsage, code ? stderr : stdout);
    std::exit(code);
}

} // namespace

int main(int argc, char** argv)
{
    azl::DeskServer::Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&] {
            if (i + 1 >= argc)
                usage(2);
            return std::atoi(argv[++i]);
        };
        if (arg == "--port")
            options.port = uint16_t(value());
        else if (arg == "--engine-port")
            options.enginePort = uint16_t(value());
        else if (arg == "--loopback-only")
            options.loopbackOnly = true;
        else if (arg == "--help" || arg == "-h")
            usage(0);
        else
            usage(2);
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::signal(SIGPIPE, SIG_IGN);
    sigset_t stop;
    sigemptyset(&stop);
    sigaddset(&stop, SIGTERM);
    sigaddset(&stop, SIGINT);
    sigaddset(&stop, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &stop, nullptr);
    try {
        azl::DeskServer desk(options);
        int signal = 0;
        sigwait(&stop, &signal);
        std::printf("desk: stopping\n");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nocturne-desk: %s\n", e.what());
        return 1;
    }
}
