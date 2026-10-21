#include "media/hap_encoder.hpp"

#include "media/hap.hpp"

#include <algorithm>
#include <stdexcept>

namespace azl {

HapCodec hapCodecNamed(const std::string& name)
{
    if (name == "hap")
        return HapCodec::Hap;
    if (name == "hap-alpha")
        return HapCodec::HapAlpha;
    if (name == "hap-q")
        return HapCodec::HapQ;
    if (name == "hap-q-alpha")
        return HapCodec::HapQAlpha;
    return HapCodec::None;
}

HapEncoder::HapEncoder(const std::filesystem::path& file, HapCodec codec, int width, int height, int fpsNum,
                       int fpsDen, int chunks)
    : writer_(file, codec, width, height, fpsNum, fpsDen), codec_(codec), width_(width), height_(height),
      chunks_(std::max(1, chunks))
{
    pending_.reserve(size_t(width) * height * 4);
}

void HapEncoder::feed(const uint8_t* data, size_t size)
{
    const size_t frameBytes = size_t(width_) * height_ * 4;
    while (size > 0) {
        const size_t take = std::min(size, frameBytes - pending_.size());
        pending_.insert(pending_.end(), data, data + take);
        data += take;
        size -= take;
        if (pending_.size() < frameBytes)
            return;
        writer_.append(encodeHapFrame(pending_.data(), width_, height_, codec_, chunks_));
        pending_.clear();
        ++frames_;
    }
}

void HapEncoder::finish()
{
    if (!pending_.empty())
        throw std::runtime_error("the input ended inside a frame (" + std::to_string(pending_.size()) + " bytes)");
    if (frames_ == 0)
        throw std::runtime_error("no frames to encode");
    writer_.finish();
}

} // namespace azl
