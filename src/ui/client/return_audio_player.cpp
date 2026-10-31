#include "ui/client/return_audio_player.hpp"

#include "protocol/return_stream.hpp"

#include <algorithm>
#include <vector>

namespace azl::ui {

namespace {

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
        start = device_ && playing_ && !output_;
    }
    if (start)
        open();
}

void ReturnAudioPlayer::open()
{
    std::unique_ptr<Output> output = openOutput([this](float* out, size_t count) { pull(out, count); });
    if (!output)
        return;
    Output& opened = *output;
    {
        std::lock_guard lock(mutex_);
        if (output_ || !playing_)
            return;
        output_ = std::move(output);
    }
    opened.start();
}

void ReturnAudioPlayer::pull(float* out, size_t count)
{
    std::lock_guard lock(mutex_);
    const float volume = float(volume_.load(std::memory_order_relaxed));
    const size_t take = playing_ ? std::min(count, buffered_.size()) : 0;
    for (size_t i = 0; i < take; ++i)
        out[i] = buffered_[i] * volume;
    std::fill(out + take, out + count, 0.0f);
    buffered_.erase(buffered_.begin(), buffered_.begin() + ptrdiff_t(take));
    if (buffered_.empty())
        playing_ = false;
}

void ReturnAudioPlayer::stop()
{
    std::unique_ptr<Output> output;
    {
        std::lock_guard lock(mutex_);
        std::swap(output, output_);
        buffered_.clear();
        playing_ = false;
        next_.reset();
        decoder_.reset();
    }
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
