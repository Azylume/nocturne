#include "ui/client/return_audio_player.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <cstdio>

namespace azl::ui {

namespace {

constexpr int kBuffers = 3;

// An AudioQueue of kBuffers buffers of one Opus frame each, filled before it starts.
class QueueOutput : public ReturnAudioPlayer::Output {
public:
    explicit QueueOutput(std::function<void(float*, size_t)> pull) : pull_(std::move(pull)) {}
    ~QueueOutput() override
    {
        if (queue_)
            AudioQueueDispose(queue_, true);
    }

    bool open()
    {
        const AudioStreamBasicDescription format{.mSampleRate = kOpusRate,
                                                 .mFormatID = kAudioFormatLinearPCM,
                                                 .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
                                                 .mBytesPerPacket = 4,
                                                 .mFramesPerPacket = 1,
                                                 .mBytesPerFrame = 4,
                                                 .mChannelsPerFrame = 1,
                                                 .mBitsPerChannel = 32};
        if (const OSStatus status = AudioQueueNewOutput(&format, fill, this, nullptr, nullptr, 0, &queue_);
            status != noErr) {
            std::fprintf(stderr, "listen: cannot open the audio output (%d)\n", int(status));
            return false;
        }
        for (int i = 0; i < kBuffers; ++i) {
            AudioQueueBufferRef buffer = nullptr;
            if (AudioQueueAllocateBuffer(queue_, UInt32(kOpusFrameSamples * 4), &buffer) == noErr)
                fill(this, queue_, buffer);
        }
        return true;
    }

    void start() override { AudioQueueStart(queue_, nullptr); }

private:
    static void fill(void* user, AudioQueueRef queue, AudioQueueBufferRef buffer)
    {
        static_cast<QueueOutput*>(user)->pull_(static_cast<float*>(buffer->mAudioData), kOpusFrameSamples);
        buffer->mAudioDataByteSize = UInt32(kOpusFrameSamples * 4);
        AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    }

    std::function<void(float*, size_t)> pull_;
    AudioQueueRef queue_ = nullptr;
};

} // namespace

std::unique_ptr<ReturnAudioPlayer::Output> ReturnAudioPlayer::openOutput(std::function<void(float*, size_t)> pull)
{
    auto output = std::make_unique<QueueOutput>(std::move(pull));
    if (!output->open())
        return nullptr;
    return output;
}

} // namespace azl::ui
