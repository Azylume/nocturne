// The child tests/platform_test.cpp starts through spawnProcess: one small job per run, the same on every platform.
#include "platform/process.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

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
    if (job == "sleep") {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    std::fprintf(stderr, "usage: nocturne_platform_child exit N | print ARGS | env NAME | beat | stdin | sleep\n");
    return 2;
}
