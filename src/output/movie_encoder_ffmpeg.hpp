#pragma once

#include "output/picture_pool_ffmpeg.hpp"
#include "output/recording.hpp"

#include <deque>
#include <string>

namespace azl::recording {

// Records with FFmpeg where AVFoundation is missing: HEVC on NVENC (hevc_nvenc), else on Media Foundation's encoder
// (hevc_mf; no GPL encoder such as libx265), ProRes 422 with prores_ks, and an AAC track with FFmpeg's own encoder,
// in QuickTime movies fragmented every 2 s. The bit rate and the colour tags are the Mac recorder's.
//
// Pictures come from newPicture() in layout(): 8-bit NV12 for HEVC, P010 for precise HEVC Main10, 10-bit planar 4:2:2
// for ProRes. The encoder never delays a frame, so each packet goes into its file as soon as its frame is encoded.
class FfmpegEncoder final : public MovieEncoder {
public:
    // `only` names the one FFmpeg encoder to try (for the tests); empty tries them in order. Throws std::runtime_error
    // when the machine cannot encode `codec` at that size.
    FfmpegEncoder(const std::string& codec, uint32_t width, uint32_t height, double fps, bool precise, Counts& counts,
                  const std::string& only = {});
    ~FfmpegEncoder() override;

    YCbCrLayout layout() const { return layout_; }
    // The FFmpeg encoder it runs: "hevc_nvenc", "hevc_mf" or "prores_ks".
    std::string name() const;
    // Never allocates or waits: a picture to fill, or null when every one is in use. One thread at a time.
    std::shared_ptr<YCbCrPicture> newPicture() { return pool_->take(); }

    std::unique_ptr<MovieFile> open(const std::filesystem::path& partial, double audioRate) override;
    bool encode(const Picture& picture, double seconds, double origin, bool key, MovieFile& file) override;
    void complete() override;
    std::optional<bool> hardware() const override { return hardware_; }

private:
    // Where each frame handed to the encoder goes, in the order they went in.
    struct Route {
        int64_t pts;
        MovieFile* file;
        int64_t origin;
    };

    bool drain();
    void lost(const Route& route);

    struct ParametersFree {
        void operator()(AVCodecParameters* p) const { avcodec_parameters_free(&p); }
    };

    Counts& counts_;
    ff::Codec codec_;
    // The codec's parameters as every file's track takes them; open() may run on another thread than encode().
    std::unique_ptr<AVCodecParameters, ParametersFree> parameters_;
    YCbCrLayout layout_ = YCbCrLayout::NV12;
    std::optional<bool> hardware_;
    std::unique_ptr<ff::PicturePool> pool_;
    std::deque<Route> routes_;
    ff::Packet packet_;
};

// Whether the movie has a video frame to play; a partial file a killed engine left counts when it does.
bool moviePlays(const std::filesystem::path& file);

} // namespace azl::recording
