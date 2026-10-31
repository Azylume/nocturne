// The child tests/platform_test.cpp starts through spawnProcess: one small job per run, the same on every platform.
#include "platform/process.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char** argv)
{
    const std::string job = argc > 1 ? argv[1] : "";
    if (job == "exit" && argc > 2)
        return std::atoi(argv[2]);
    if (job == "print") {
        for (int i = 2; i < argc; ++i)
            std::printf("[%s]", argv[i]);
        std::fflush(stdout);
        std::fprintf(stderr, "on stderr");
        return 0;
    }
    if (job == "env" && argc > 2) {
        const char* value = std::getenv(argv[2]);
        std::printf("%s", value ? value : "<unset>");
        return 0;
    }
    if (job == "beat") {
        const std::optional<azl::NativeHandle> heartbeat = azl::inheritedHeartbeat();
        return heartbeat && azl::writeHandle(*heartbeat, "beat", 4) == 4 ? 0 : 3;
    }
    if (job == "stdin") {
        size_t bytes = 0;
        while (std::fgetc(stdin) != EOF)
            ++bytes;
        std::printf("%zu", bytes);
        return 0;
    }
    if (job == "stop") {
        if (!azl::watchStopRequests())
            return 5;
        std::printf("watching\n");
        std::fflush(stdout);
        return azl::waitForStop(30.0) ? 0 : 4;
    }
    if (job == "fault") {
#ifdef _WIN32
        SetErrorMode(SEM_NOGPFAULTERRORBOX);
#endif
        int* volatile nowhere = nullptr;
        *nowhere = 1;
        return 0;
    }
    if (job == "sleep") {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    std::fprintf(stderr, "usage: nocturne_platform_child exit N | print ARGS | env NAME | beat | stdin | stop | fault | sleep\n");
    return 2;
}
