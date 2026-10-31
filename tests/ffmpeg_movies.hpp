#pragma once

#include "media/ffmpeg_util.hpp"
#include "output/ycbcr_picture.hpp"

#include <doctest.h>

extern "C" {
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

// Test pictures in and out of FFmpeg encoders: RGB cards converted to the encoders' Y'CbCr layouts on the CPU (as the
// GPU's Y'CbCr pass will), and movies decoded back with libavformat and libavcodec.
namespace {

struct Rgb {
    uint32_t width = 0;
    uint32_t height = 0;
    // Rows of width * 3 bytes.
    std::vector<uint8_t> rgb;
};

// Smooth ramps with bars across them and a white marker that moves 8 pixels a frame.
[[maybe_unused]] Rgb card(uint32_t width, uint32_t height, int frame)
{
    Rgb card{width, height, std::vector<uint8_t>(size_t(width) * height * 3)};
    const uint32_t markerX = uint32_t(frame * 8) % (width - 32);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* p = &card.rgb[(size_t(y) * width + x) * 3];
            const bool bar = (x / 40) % 3 == 0;
            p[0] = uint8_t(x * 255 / width);
            p[1] = uint8_t(bar ? 40 : y * 255 / height);
            p[2] = uint8_t(bar ? 200 : 255 - x * 255 / width);
            if (x >= markerX && x < markerX + 32 && y >= height / 3 && y < height / 3 + 32)
                p[0] = p[1] = p[2] = 255;
        }
    return card;
}

// BT.709 Y', Cb and Cr of a pixel, each 0..1 (Cb and Cr centred on 0.5).
[[maybe_unused]] std::array<double, 3> ycbcrOf(const uint8_t* p)
{
    const double r = p[0] / 255.0, g = p[1] / 255.0, b = p[2] / 255.0;
    const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    return {y, 0.5 + (b - y) / 1.8556, 0.5 + (r - y) / 1.5748};
}

// Video range code values: 8 or 10 bits.
[[maybe_unused]] int lumaCode(double y, int bits)
{
    return int(std::lround((16.0 + 219.0 * y) * double(1 << (bits - 8))));
}
[[maybe_unused]] int chromaCode(double c, int bits)
{
    return int(std::lround((16.0 + 224.0 * c) * double(1 << (bits - 8))));
}

// Writes `card` into `picture`, chroma averaged over each 2x2 (4:2:0) or 2x1 (4:2:2) block.
[[maybe_unused]] void fill(azl::YCbCrPicture& picture, const Rgb& card)
{
    REQUIRE(picture.width == card.width);
    REQUIRE(picture.height == card.height);
    const bool planar = picture.layout == azl::YCbCrLayout::Planar422P10;
    const int bits = picture.layout == azl::YCbCrLayout::NV12 ? 8 : 10;
    const uint32_t blockRows = planar ? 1 : 2;
    auto store = [&](int plane, uint32_t x, uint32_t y, int code) {
        uint8_t* row = picture.planes[size_t(plane)] + size_t(y) * picture.strides[size_t(plane)];
        if (picture.layout == azl::YCbCrLayout::NV12)
            row[x] = uint8_t(code);
        else if (picture.layout == azl::YCbCrLayout::P010)
            reinterpret_cast<uint16_t*>(row)[x] = uint16_t(code << 6);
        else
            reinterpret_cast<uint16_t*>(row)[x] = uint16_t(code);
    };
    for (uint32_t y = 0; y < card.height; ++y)
        for (uint32_t x = 0; x < card.width; ++x)
            store(0, x, y, lumaCode(ycbcrOf(&card.rgb[(size_t(y) * card.width + x) * 3])[0], bits));
    for (uint32_t y = 0; y < card.height / blockRows; ++y)
        for (uint32_t x = 0; x < card.width / 2; ++x) {
            double cb = 0.0, cr = 0.0;
            for (uint32_t dy = 0; dy < blockRows; ++dy)
                for (uint32_t dx = 0; dx < 2; ++dx) {
                    const auto c = ycbcrOf(&card.rgb[(size_t(y * blockRows + dy) * card.width + x * 2 + dx) * 3]);
                    cb += c[1];
                    cr += c[2];
                }
            const double n = 2.0 * blockRows;
            if (planar) {
                store(1, x, y, chromaCode(cb / n, bits));
                store(2, x, y, chromaCode(cr / n, bits));
            } else {
                store(1, x * 2, y, chromaCode(cb / n, bits));
                store(1, x * 2 + 1, y, chromaCode(cr / n, bits));
            }
        }
}

[[maybe_unused]] double meanError(const Rgb& a, const Rgb& b)
{
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    uint64_t sum = 0;
    for (size_t i = 0; i < a.rgb.size(); ++i)
        sum += uint64_t(std::abs(int(a.rgb[i]) - int(b.rgb[i])));
    return double(sum) / double(a.rgb.size());
}

// A decoded picture as RGB, through BT.709 video range like a player.
[[maybe_unused]] Rgb toRgb(const AVFrame* frame)
{
    Rgb out{uint32_t(frame->width), uint32_t(frame->height),
            std::vector<uint8_t>(size_t(frame->width) * frame->height * 3)};
    azl::ff::Scale scale(sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format), frame->width,
                                        frame->height, AV_PIX_FMT_RGB24, SWS_POINT, nullptr, nullptr, nullptr));
    REQUIRE(scale);
    const int* bt709 = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(scale.get(), bt709, 0, bt709, 1, 0, 1 << 16, 1 << 16);
    // swscale may write past a row's last pixel, so it writes into rows padded to 64 bytes and one row more.
    const size_t row = size_t(out.width) * 3;
    const size_t padded = (row + 63) / 64 * 64;
    std::vector<uint8_t> scaled(padded * (out.height + 1));
    uint8_t* planes[1] = {scaled.data()};
    const int strides[1] = {int(padded)};
    sws_scale(scale.get(), frame->data, frame->linesize, 0, frame->height, planes, strides);
    for (uint32_t y = 0; y < out.height; ++y)
        std::memcpy(&out.rgb[y * row], &scaled[y * padded], row);
    return out;
}

struct Movie {
    AVCodecID codec = AV_CODEC_ID_NONE;
    // The QuickTime four-character code, most significant byte first.
    uint32_t tag = 0;
    int profile = 0;
    AVPixelFormat format = AV_PIX_FMT_NONE;
    std::vector<double> times;
    std::vector<Rgb> frames;
    // Of the video packets, in file order.
    std::vector<bool> keys;
    int audioTracks = 0;
    AVCodecID audioCodec = AV_CODEC_ID_NONE;
    int audioRate = 0;
    std::vector<float> audio;
    // The tracks' lengths as a player plays them: their packets' durations, less the audio priming it skips.
    double videoSeconds = 0.0;
    double audioSeconds = 0.0;
};

// The samples a player skips at the start of an audio packet.
[[maybe_unused]] uint32_t skippedSamples(const AVPacket* packet)
{
    size_t size = 0;
    const uint8_t* skip = av_packet_get_side_data(packet, AV_PKT_DATA_SKIP_SAMPLES, &size);
    return skip && size >= 4 ? uint32_t(skip[0]) | uint32_t(skip[1]) << 8 | uint32_t(skip[2]) << 16 |
                                   uint32_t(skip[3]) << 24
                             : 0;
}

[[maybe_unused]] Movie decodeMovie(const std::filesystem::path& file)
{
    Movie movie;
    int error = 0;
    const azl::ff::Format format = azl::ff::openFormat(file, error);
    REQUIRE_MESSAGE(format, azl::ff::describe(error));
    REQUIRE(avformat_find_stream_info(format.get(), nullptr) >= 0);
    const int video = azl::ff::videoStream(format.get());
    REQUIRE(video >= 0);
    int audio = -1;
    for (unsigned i = 0; i < format->nb_streams; ++i)
        if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            movie.audioTracks++;
            audio = int(i);
        }
    auto open = [&](int index) {
        const AVCodecParameters* parameters = format->streams[index]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
        REQUIRE(codec);
        azl::ff::Codec context(avcodec_alloc_context3(codec));
        REQUIRE(avcodec_parameters_to_context(context.get(), parameters) >= 0);
        context->pkt_timebase = format->streams[index]->time_base;
        REQUIRE(avcodec_open2(context.get(), codec, nullptr) >= 0);
        return context;
    };
    const AVCodecParameters* parameters = format->streams[video]->codecpar;
    movie.codec = parameters->codec_id;
    movie.tag = azl::ff::fourccOfTag(parameters->codec_tag);
    movie.profile = parameters->profile;
    movie.format = AVPixelFormat(parameters->format);
    azl::ff::Codec videoCodec = open(video);
    azl::ff::Codec audioCodec;
    if (audio >= 0) {
        audioCodec = open(audio);
        movie.audioCodec = format->streams[audio]->codecpar->codec_id;
        movie.audioRate = format->streams[audio]->codecpar->sample_rate;
    }
    const double timeBase = av_q2d(format->streams[video]->time_base);
    azl::ff::Packet packet(av_packet_alloc());
    azl::ff::Frame frame(av_frame_alloc());
    auto receive = [&](AVCodecContext* codec, bool isVideo) {
        while (avcodec_receive_frame(codec, frame.get()) >= 0) {
            if (isVideo) {
                movie.times.push_back(double(frame->best_effort_timestamp) * timeBase);
                movie.frames.push_back(toRgb(frame.get()));
            } else {
                const auto* samples = reinterpret_cast<const float*>(frame->extended_data[0]);
                REQUIRE(frame->format == AV_SAMPLE_FMT_FLTP);
                movie.audio.insert(movie.audio.end(), samples, samples + frame->nb_samples);
            }
            av_frame_unref(frame.get());
        }
    };
    while (av_read_frame(format.get(), packet.get()) >= 0) {
        if (packet->stream_index == video) {
            movie.videoSeconds += double(packet->duration) * timeBase;
            movie.keys.push_back(packet->flags & AV_PKT_FLAG_KEY);
            REQUIRE(avcodec_send_packet(videoCodec.get(), packet.get()) >= 0);
            receive(videoCodec.get(), true);
        } else if (packet->stream_index == audio) {
            movie.audioSeconds += double(packet->duration) * av_q2d(format->streams[audio]->time_base) -
                                  double(skippedSamples(packet.get())) / movie.audioRate;
            REQUIRE(avcodec_send_packet(audioCodec.get(), packet.get()) >= 0);
            receive(audioCodec.get(), false);
        }
        av_packet_unref(packet.get());
    }
    avcodec_send_packet(videoCodec.get(), nullptr);
    receive(videoCodec.get(), true);
    if (audioCodec) {
        avcodec_send_packet(audioCodec.get(), nullptr);
        receive(audioCodec.get(), false);
    }
    return movie;
}

} // namespace
