#include "audio/opus_codec.hpp"

#include <opus.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace azl {

namespace {

constexpr size_t kMaxPacket = 1500;
// The longest frame a packet may hold: 120 ms.
constexpr size_t kMaxFrameSamples = 5760;

[[noreturn]] void fail(const char* what, int error)
{
    throw std::runtime_error(std::string(what) + " (" + opus_strerror(error) + ")");
}

} // namespace

// Resamples to kOpusRate by linear interpolation, which is enough for a monitoring stream; a 48 kHz input passes
// through untouched.
struct OpusStreamEncoder::Impl {
    double inputRate;
    double step;
    OpusEncoder* encoder = nullptr;
    // Input not yet resampled; `position` is where the next output sample falls in it.
    std::vector<float> input;
    double position = 0.0;
    std::vector<float> frame;

    void resample(const float* samples, size_t count)
    {
        if (step == 1.0) {
            frame.insert(frame.end(), samples, samples + count);
            return;
        }
        input.insert(input.end(), samples, samples + count);
        for (; position + 1.0 < double(input.size()); position += step) {
            const size_t i = size_t(position);
            const float t = float(position - double(i));
            frame.push_back(input[i] + (input[i + 1] - input[i]) * t);
        }
        const size_t used = std::min(size_t(position), input.size());
        input.erase(input.begin(), input.begin() + ptrdiff_t(used));
        position -= double(used);
    }
};

OpusStreamEncoder::OpusStreamEncoder(double inputRate, int bitrate) : impl_(std::make_unique<Impl>())
{
    impl_->inputRate = inputRate;
    impl_->step = inputRate / kOpusRate;
    int error = OPUS_OK;
    impl_->encoder = opus_encoder_create(opus_int32(kOpusRate), 1, OPUS_APPLICATION_AUDIO, &error);
    if (error != OPUS_OK)
        fail("cannot open the Opus encoder", error);
    opus_encoder_ctl(impl_->encoder, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(impl_->encoder, OPUS_SET_VBR_CONSTRAINT(1));
}

OpusStreamEncoder::~OpusStreamEncoder()
{
    opus_encoder_destroy(impl_->encoder);
}

double OpusStreamEncoder::inputRate() const
{
    return impl_->inputRate;
}

std::vector<std::vector<uint8_t>> OpusStreamEncoder::encode(const float* samples, size_t count)
{
    Impl& impl = *impl_;
    impl.resample(samples, count);
    std::vector<std::vector<uint8_t>> packets;
    size_t at = 0;
    for (; at + kOpusFrameSamples <= impl.frame.size(); at += kOpusFrameSamples) {
        std::vector<uint8_t> packet(kMaxPacket);
        const opus_int32 size = opus_encode_float(impl.encoder, impl.frame.data() + at, int(kOpusFrameSamples),
                                                  packet.data(), opus_int32(packet.size()));
        if (size < 0)
            break;
        packet.resize(size_t(size));
        packets.push_back(std::move(packet));
    }
    impl.frame.erase(impl.frame.begin(), impl.frame.begin() + ptrdiff_t(at));
    return packets;
}

struct OpusStreamDecoder::Impl {
    OpusDecoder* decoder = nullptr;
};

OpusStreamDecoder::OpusStreamDecoder() : impl_(std::make_unique<Impl>())
{
    int error = OPUS_OK;
    impl_->decoder = opus_decoder_create(opus_int32(kOpusRate), 1, &error);
    if (error != OPUS_OK)
        fail("cannot open the Opus decoder", error);
}

OpusStreamDecoder::~OpusStreamDecoder()
{
    opus_decoder_destroy(impl_->decoder);
}

bool OpusStreamDecoder::decode(const uint8_t* packet, size_t size, std::vector<float>& out)
{
    const size_t start = out.size();
    out.resize(start + kMaxFrameSamples);
    const int frames =
        opus_decode_float(impl_->decoder, packet, opus_int32(size), out.data() + start, int(kMaxFrameSamples), 0);
    out.resize(start + size_t(std::max(frames, 0)));
    return frames >= 0;
}

} // namespace azl
