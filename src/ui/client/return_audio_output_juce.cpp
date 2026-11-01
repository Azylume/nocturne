#include "ui/client/return_audio_player.hpp"

#include <juce_audio_devices/juce_audio_devices.h>

#include <cmath>
#include <cstdio>

namespace azl::ui {

namespace {

// JUCE's default output device, asked for kOpusRate. A device that runs at another rate gets the samples through a
// Lagrange resampler; every channel plays the same mono signal.
class JuceOutput : public ReturnAudioPlayer::Output, private juce::AudioIODeviceCallback {
public:
    explicit JuceOutput(std::function<void(float*, size_t)> pull) : pull_(std::move(pull)) {}
    ~JuceOutput() override
    {
        devices_.removeAudioCallback(this);
        devices_.closeAudioDevice();
    }

    bool open()
    {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.sampleRate = kOpusRate;
        const juce::String error = devices_.initialise(0, 2, nullptr, true, {}, &setup);
        if (error.isNotEmpty() || !devices_.getCurrentAudioDevice()) {
            std::fprintf(stderr, "listen: cannot open the audio output (%s)\n",
                         error.isNotEmpty() ? error.toRawUTF8() : "no output device");
            return false;
        }
        return true;
    }

    void start() override { devices_.addAudioCallback(this); }

private:
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        ratio_ = kOpusRate / device->getCurrentSampleRate();
        resampler_.reset();
        const size_t most = size_t(std::ceil(device->getCurrentBufferSizeSamples() * ratio_)) + kSlack;
        mono_.assign(size_t(device->getCurrentBufferSizeSamples()), 0.0f);
        input_.assign(most * 2, 0.0f);
        held_ = 0;
    }

    void audioDeviceStopped() override {}

    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int channels, int samples,
                                          const juce::AudioIODeviceCallbackContext&) override
    {
        if (size_t(samples) > mono_.size()) {
            for (int channel = 0; channel < channels; ++channel)
                if (outputs[channel])
                    std::fill_n(outputs[channel], samples, 0.0f);
            return;
        }
        if (ratio_ == 1.0)
            pull_(mono_.data(), size_t(samples));
        else
            resample(samples);
        for (int channel = 0; channel < channels; ++channel)
            if (outputs[channel])
                std::copy_n(mono_.data(), samples, outputs[channel]);
    }

    // Pulls what the resampler needs for `samples` output samples; what it leaves stays for the next callback.
    void resample(int samples)
    {
        const size_t needed = size_t(std::ceil(samples * ratio_)) + kSlack;
        if (held_ < needed) {
            pull_(input_.data() + held_, needed - held_);
            held_ = needed;
        }
        const int used = resampler_.process(ratio_, input_.data(), mono_.data(), samples);
        std::copy(input_.begin() + used, input_.begin() + ptrdiff_t(held_), input_.begin());
        held_ -= size_t(used);
    }

    static constexpr size_t kSlack = 4;
    std::function<void(float*, size_t)> pull_;
    juce::AudioDeviceManager devices_;
    juce::LagrangeInterpolator resampler_;
    double ratio_ = 1.0;
    std::vector<float> mono_;
    std::vector<float> input_;
    size_t held_ = 0;
};

} // namespace

std::unique_ptr<ReturnAudioPlayer::Output> ReturnAudioPlayer::openOutput(std::function<void(float*, size_t)> pull)
{
    auto output = std::make_unique<JuceOutput>(std::move(pull));
    if (!output->open())
        return nullptr;
    return output;
}

} // namespace azl::ui
