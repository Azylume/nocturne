#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

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
    doctest::Context context(argc, argv);
    return context.run();
}
