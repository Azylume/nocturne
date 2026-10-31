#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include <juce_gui_basics/juce_gui_basics.h>

#if !defined(__APPLE__)
#include "gpu/gpu_vulkan.hpp"
#endif

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
#ifdef _WIN32
    _putenv_s("NOCTURNE_NO_AUDIO_INPUT", "1");
    // The Vulkan validation layer checks every GPU call the engines in these tests make; NOCTURNE_VULKAN_VALIDATION=0
    // turns it off.
    if (!std::getenv("NOCTURNE_VULKAN_VALIDATION"))
        _putenv_s("NOCTURNE_VULKAN_VALIDATION", "1");
#else
    setenv("NOCTURNE_NO_AUDIO_INPUT", "1", 1);
#if !defined(__APPLE__)
    setenv("NOCTURNE_VULKAN_VALIDATION", "1", 0);
#endif
#endif
    juce::ScopedJuceInitialiser_GUI gui;
#if JUCE_MAC
    juce::Process::setDockIconVisible(false);
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
