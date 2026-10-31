#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#if !defined(__APPLE__)
#include "gpu/gpu_vulkan.hpp"
#endif

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv)
{
    // Every engine these tests start keeps its audio input off, so no test asks for the microphone.
    // NOCTURNE_REAL_AUDIO=1 runs the tests that need a real input.
    const char* real = std::getenv("NOCTURNE_REAL_AUDIO");
    if (!real || std::string(real) != "1")
#ifdef _WIN32
        _putenv_s("NOCTURNE_NO_AUDIO_INPUT", "1");
#else
        setenv("NOCTURNE_NO_AUDIO_INPUT", "1", 1);
#endif
#if !defined(__APPLE__)
    // The Vulkan validation layer checks every GPU call the tests make; NOCTURNE_VULKAN_VALIDATION=0 turns it off.
    if (!std::getenv("NOCTURNE_VULKAN_VALIDATION"))
#ifdef _WIN32
        _putenv_s("NOCTURNE_VULKAN_VALIDATION", "1");
#else
        setenv("NOCTURNE_VULKAN_VALIDATION", "1", 1);
#endif
#endif
    doctest::Context context(argc, argv);
    int result = context.run();
#if !defined(__APPLE__)
    if (const uint64_t errors = azl::gpu::vulkan::validationErrors()) {
        std::fprintf(stderr, "Vulkan validation errors: %llu\n", static_cast<unsigned long long>(errors));
        result = result ? result : 1;
    }
#endif
    return result;
}
