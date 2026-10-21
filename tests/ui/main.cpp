#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdlib>

int main(int argc, char** argv)
{
    setenv("NOCTURNE_NO_AUDIO_INPUT", "1", 1);
    juce::ScopedJuceInitialiser_GUI gui;
    juce::Process::setDockIconVisible(false);
    doctest::Context context(argc, argv);
    return context.run();
}
