#include "audio/opus_codec.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace azl {

namespace {

// An input proc's answer for "nothing more until the next call": the converter keeps its state.
constexpr OSStatus kNoDataNow = 'wait';
constexpr size_t kMaxSupply = 4096;
constexpr UInt32 kPacketsPerFill = 8;

AudioStreamBasicDescription monoFloat(double rate)
{
    return {.mSampleRate = rate,
            .mFormatID = kAudioFormatLinearPCM,
            .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
            .mBytesPerPacket = 4,
            .mFramesPerPacket = 1,
            .mBytesPerFrame = 4,
            .mChannelsPerFrame = 1,
            .mBitsPerChannel = 32};
}

AudioStreamBasicDescription opus()
{
    return {.mSampleRate = kOpusRate,
            .mFormatID = kAudioFormatOpus,
            .mFramesPerPacket = UInt32(kOpusFrameSamples),
            .mChannelsPerFrame = 1};
}

AudioConverterRef converter(const AudioStreamBasicDescription& from, const AudioStreamBasicDescription& to)
{
    AudioConverterRef converter = nullptr;
    if (const OSStatus status = AudioConverterNew(&from, &to, &converter); status != noErr)
        throw std::runtime_error("no Opus codec on this system (" + std::to_string(status) + ")");
    return converter;
}

} // namespace

struct OpusStreamEncoder::Impl {
    double inputRate;
    AudioConverterRef converter;
    UInt32 maxPacket = 0;
    std::vector<float> pending;
    size_t taken = 0;
    // What the converter reads from until its next input call.
    std::vector<float> supplied = std::vector<float>(kMaxSupply);

    static OSStatus supply(AudioConverterRef, UInt32* frames, AudioBufferList* data, AudioStreamPacketDescription**,
                           void* user)
    {
        Impl& self = *static_cast<Impl*>(user);
        const size_t n = std::min({size_t(*frames), self.pending.size() - self.taken, kMaxSupply});
        *frames = UInt32(n);
        if (n == 0)
            return kNoDataNow;
        std::copy_n(self.pending.begin() + ptrdiff_t(self.taken), n, self.supplied.begin());
        self.taken += n;
        data->mBuffers[0] = {.mNumberChannels = 1, .mDataByteSize = UInt32(n * 4), .mData = self.supplied.data()};
        return noErr;
    }
};

OpusStreamEncoder::OpusStreamEncoder(double inputRate, int bitrate) : impl_(std::make_unique<Impl>())
{
    impl_->inputRate = inputRate;
    impl_->converter = converter(monoFloat(inputRate), opus());
    const UInt32 rate = UInt32(bitrate);
    AudioConverterSetProperty(impl_->converter, kAudioConverterEncodeBitRate, sizeof rate, &rate);
    UInt32 size = sizeof impl_->maxPacket;
    AudioConverterGetProperty(impl_->converter, kAudioConverterPropertyMaximumOutputPacketSize, &size, &impl_->maxPacket);
    impl_->maxPacket = std::max<UInt32>(impl_->maxPacket, 1500);
}

OpusStreamEncoder::~OpusStreamEncoder()
{
    AudioConverterDispose(impl_->converter);
}

double OpusStreamEncoder::inputRate() const
{
    return impl_->inputRate;
}

std::vector<std::vector<uint8_t>> OpusStreamEncoder::encode(const float* samples, size_t count)
{
    Impl& impl = *impl_;
    impl.pending.erase(impl.pending.begin(), impl.pending.begin() + ptrdiff_t(impl.taken));
    impl.taken = 0;
    impl.pending.insert(impl.pending.end(), samples, samples + count);

    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint8_t> out(size_t(impl.maxPacket) * kPacketsPerFill);
    AudioStreamPacketDescription descriptions[kPacketsPerFill];
    for (;;) {
        AudioBufferList list{.mNumberBuffers = 1,
                             .mBuffers = {{.mNumberChannels = 1, .mDataByteSize = UInt32(out.size()), .mData = out.data()}}};
        UInt32 produced = kPacketsPerFill;
        const OSStatus status =
            AudioConverterFillComplexBuffer(impl.converter, Impl::supply, &impl, &produced, &list, descriptions);
        for (UInt32 i = 0; i < produced; ++i) {
            const auto begin = out.begin() + ptrdiff_t(descriptions[i].mStartOffset);
            packets.emplace_back(begin, begin + ptrdiff_t(descriptions[i].mDataByteSize));
        }
        if (status != noErr || produced < kPacketsPerFill)
            return packets;
    }
}

struct OpusStreamDecoder::Impl {
    AudioConverterRef converter;
    const uint8_t* packet = nullptr;
    UInt32 size = 0;
    AudioStreamPacketDescription description{};

    static OSStatus supply(AudioConverterRef, UInt32* packets, AudioBufferList* data,
                           AudioStreamPacketDescription** descriptions, void* user)
    {
        Impl& self = *static_cast<Impl*>(user);
        if (!self.packet) {
            *packets = 0;
            return kNoDataNow;
        }
        *packets = 1;
        data->mBuffers[0] = {.mNumberChannels = 1, .mDataByteSize = self.size, .mData = const_cast<uint8_t*>(self.packet)};
        self.description = {.mStartOffset = 0, .mVariableFramesInPacket = 0, .mDataByteSize = self.size};
        if (descriptions)
            *descriptions = &self.description;
        self.packet = nullptr;
        return noErr;
    }
};

OpusStreamDecoder::OpusStreamDecoder() : impl_(std::make_unique<Impl>())
{
    impl_->converter = converter(opus(), monoFloat(kOpusRate));
}

OpusStreamDecoder::~OpusStreamDecoder()
{
    AudioConverterDispose(impl_->converter);
}

bool OpusStreamDecoder::decode(const uint8_t* packet, size_t size, std::vector<float>& out)
{
    Impl& impl = *impl_;
    impl.packet = packet;
    impl.size = UInt32(size);
    const size_t start = out.size();
    out.resize(start + 2 * kOpusFrameSamples);
    AudioBufferList list{.mNumberBuffers = 1,
                         .mBuffers = {{.mNumberChannels = 1,
                                       .mDataByteSize = UInt32(2 * kOpusFrameSamples * 4),
                                       .mData = out.data() + start}}};
    UInt32 frames = UInt32(2 * kOpusFrameSamples);
    const OSStatus status = AudioConverterFillComplexBuffer(impl.converter, Impl::supply, &impl, &frames, &list, nullptr);
    out.resize(start + frames);
    impl.packet = nullptr;
    return status == noErr || status == kNoDataNow;
}

} // namespace azl
