#include "ui/client/return_audio_player.hpp"

#include "protocol/return_stream.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace azl::ui {

namespace {

constexpr int kBuffers = 3;

size_t samplesOf(double seconds)
{
    return size_t(seconds * kOpusRate);
}

} // namespace

ReturnAudioPlayer::~ReturnAudioPlayer()
{
    stop();
}

void ReturnAudioPlayer::receive(const Message& message)
{
    if (message.value("op", "") != "audio" || !isReturnPacket(message))
        return;
    bool start = false;
    {
        std::lock_guard lock(mutex_);
        if (!decoder_)
            decoder_ = std::make_unique<OpusStreamDecoder>();
        const auto& sizes = message["sizes"];
        const uint64_t seq = message.value("seq", uint64_t(0));
        if (next_ && seq > *next_)
            lost_.fetch_add((seq - *next_) * sizes.size(), std::memory_order_relaxed);
        next_ = seq + 1;
        const auto& payload = message[kReturnPayload].get_binary();
        std::vector<float> pcm;
        size_t offset = 0;
        for (const size_t size : sizes) {
            if (offset + size > payload.size())
                break;
            decoder_->decode(payload.data() + offset, size, pcm);
            offset += size;
        }
        decoded_.fetch_add(pcm.size(), std::memory_order_relaxed);
        buffered_.insert(buffered_.end(), pcm.begin(), pcm.end());
        if (buffered_.size() > samplesOf(kMaxSeconds))
            buffered_.erase(buffered_.begin(), buffered_.end() - ptrdiff_t(samplesOf(kStartSeconds)));
        if (!playing_ && buffered_.size() >= samplesOf(kStartSeconds))
            playing_ = true;
        start = device_ && playing_ && !queue_;
    }
    if (start)
        open();
}

void ReturnAudioPlayer::open()
{
    const AudioStreamBasicDescription format{.mSampleRate = kOpusRate,
                                             .mFormatID = kAudioFormatLinearPCM,
                                             .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
                                             .mBytesPerPacket = 4,
                                             .mFramesPerPacket = 1,
                                             .mBytesPerFrame = 4,
                                             .mChannelsPerFrame = 1,
                                             .mBitsPerChannel = 32};
    AudioQueueRef queue = nullptr;
    if (const OSStatus status = AudioQueueNewOutput(&format, fill, this, nullptr, nullptr, 0, &queue); status != noErr) {
        std::fprintf(stderr, "listen: cannot open the audio output (%d)\n", int(status));
        return;
    }
    for (int i = 0; i < kBuffers; ++i) {
        AudioQueueBufferRef buffer = nullptr;
        if (AudioQueueAllocateBuffer(queue, UInt32(kOpusFrameSamples * 4), &buffer) == noErr)
            fill(this, queue, buffer);
    }
    {
        std::lock_guard lock(mutex_);
        if (queue_ || !playing_) {
            AudioQueueDispose(queue, true);
            return;
        }
        queue_ = queue;
    }
    AudioQueueStart(queue, nullptr);
}

void ReturnAudioPlayer::fill(void* user, OpaqueAudioQueue* queue, AudioQueueBuffer* buffer)
{
    auto& self = *static_cast<ReturnAudioPlayer*>(user);
    auto* out = static_cast<float*>(buffer->mAudioData);
    const size_t n = kOpusFrameSamples;
    {
        std::lock_guard lock(self.mutex_);
        const float volume = float(self.volume_.load(std::memory_order_relaxed));
        const size_t take = self.playing_ ? std::min(n, self.buffered_.size()) : 0;
        for (size_t i = 0; i < take; ++i)
            out[i] = self.buffered_[i] * volume;
        std::fill(out + take, out + n, 0.0f);
        self.buffered_.erase(self.buffered_.begin(), self.buffered_.begin() + ptrdiff_t(take));
        if (self.buffered_.empty())
            self.playing_ = false;
    }
    buffer->mAudioDataByteSize = UInt32(n * 4);
    AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
}

void ReturnAudioPlayer::stop()
{
    AudioQueueRef queue = nullptr;
    {
        std::lock_guard lock(mutex_);
        std::swap(queue, queue_);
        buffered_.clear();
        playing_ = false;
        next_.reset();
        decoder_.reset();
    }
    if (queue)
        AudioQueueDispose(queue, true);
}

void ReturnAudioPlayer::setVolume(double volume)
{
    volume_.store(std::clamp(volume, 0.0, 1.0), std::memory_order_relaxed);
}

double ReturnAudioPlayer::volume() const
{
    return volume_.load(std::memory_order_relaxed);
}

} // namespace azl::ui
