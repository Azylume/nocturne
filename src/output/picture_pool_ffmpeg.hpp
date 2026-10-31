#pragma once

#include "media/ffmpeg_util.hpp"
#include "output/ycbcr_picture.hpp"

extern "C" {
#include <libavutil/frame.h>
}

#include <memory>
#include <stdexcept>
#include <vector>

namespace azl::ff {

inline AVPixelFormat pixelFormatOf(YCbCrLayout layout)
{
    switch (layout) {
    case YCbCrLayout::NV12: return AV_PIX_FMT_NV12;
    case YCbCrLayout::P010: return AV_PIX_FMT_P010LE;
    case YCbCrLayout::Planar422P10: return AV_PIX_FMT_YUV422P10LE;
    }
    return AV_PIX_FMT_NONE;
}

// A picture of a PicturePool: its planes are its frame's.
struct PooledPicture : YCbCrPicture {
    Frame frame;
};

// A fixed set of frames an encoder takes as they are, allocated up front. A picture is free again once nobody holds it
// and the encoder let go of its frame; take() never allocates or waits, so the render thread may call it.
class PicturePool {
public:
    // Throws std::runtime_error when the frames cannot be allocated.
    PicturePool(YCbCrLayout layout, uint32_t width, uint32_t height, int count)
    {
        for (int i = 0; i < count; ++i) {
            auto picture = std::make_shared<PooledPicture>();
            picture->frame.reset(av_frame_alloc());
            AVFrame* frame = picture->frame.get();
            if (!frame)
                throw std::runtime_error("cannot allocate encoder pictures");
            frame->format = pixelFormatOf(layout);
            frame->width = int(width);
            frame->height = int(height);
            if (av_frame_get_buffer(frame, 0) < 0)
                throw std::runtime_error("cannot allocate encoder pictures");
            picture->layout = layout;
            picture->width = width;
            picture->height = height;
            picture->planeCount = layout == YCbCrLayout::Planar422P10 ? 3 : 2;
            for (int p = 0; p < picture->planeCount; ++p) {
                picture->planes[size_t(p)] = frame->data[p];
                picture->strides[size_t(p)] = size_t(frame->linesize[p]);
            }
            pictures_.push_back(std::move(picture));
        }
    }

    // One thread at a time. A free picture, or null when every one is in use.
    std::shared_ptr<YCbCrPicture> take()
    {
        for (const std::shared_ptr<PooledPicture>& picture : pictures_)
            if (picture.use_count() == 1 && av_frame_is_writable(picture->frame.get()))
                return picture;
        return nullptr;
    }

    // The frame of a picture this pool (any pool) handed out.
    static AVFrame* frameOf(const YCbCrPicture& picture) { return static_cast<const PooledPicture&>(picture).frame.get(); }

private:
    std::vector<std::shared_ptr<PooledPicture>> pictures_;
};

} // namespace azl::ff
