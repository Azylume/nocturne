#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <filesystem>
#include <memory>
#include <string>

// Owning handles for FFmpeg objects and the few steps every FFmpeg reader shares.
namespace azl::ff {

struct FormatFree {
    void operator()(AVFormatContext* f) const { avformat_close_input(&f); }
};
struct CodecFree {
    void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};
struct PacketFree {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};
struct FrameFree {
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};
struct ScaleFree {
    void operator()(SwsContext* s) const { sws_freeContext(s); }
};
using Format = std::unique_ptr<AVFormatContext, FormatFree>;
using Codec = std::unique_ptr<AVCodecContext, CodecFree>;
using Packet = std::unique_ptr<AVPacket, PacketFree>;
using Frame = std::unique_ptr<AVFrame, FrameFree>;
using Scale = std::unique_ptr<SwsContext, ScaleFree>;

inline std::string describe(int error)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, text, sizeof(text));
    return text;
}

// The file's demuxer, or null with `error` set. Paths are UTF-8, which FFmpeg's file protocol expects on every
// platform.
inline Format openFormat(const std::filesystem::path& file, int& error)
{
    AVFormatContext* raw = nullptr;
    const std::u8string path = file.u8string();
    error = avformat_open_input(&raw, reinterpret_cast<const char*>(path.c_str()), nullptr, nullptr);
    return Format(error < 0 ? nullptr : raw);
}

// The index of the file's first video stream, or -1. Attached pictures (cover art) are not video.
inline int videoStream(const AVFormatContext* format)
{
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVStream* stream = format->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
            return int(i);
    }
    return -1;
}

// The QuickTime four-character code of a stream's codec tag, most significant byte first.
inline uint32_t fourccOfTag(uint32_t tag)
{
    return (tag & 0xff) << 24 | (tag >> 8 & 0xff) << 16 | (tag >> 16 & 0xff) << 8 | tag >> 24;
}

} // namespace azl::ff
