#include "output/recorder_session.hpp"

#include "output/movie_encoder_ffmpeg.hpp"
#include "output/ycbcr_pass.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace azl {

namespace {

// Pictures on their way through the GPU at once; one more frame is dropped.
constexpr size_t kSlots = 4;

const char* nameOf(YCbCrLayout layout)
{
    switch (layout) {
    case YCbCrLayout::NV12:
        return "nv12";
    case YCbCrLayout::P010:
        return "p010le";
    case YCbCrLayout::Planar422P10:
        return "yuv422p10le";
    }
    return "";
}

// The planes of one picture on the GPU, and where the CPU reads them.
struct Slot {
    std::vector<gpu::Texture> planes;
    std::vector<gpu::Readback> readbacks;
    std::atomic<bool> busy{false};
};

} // namespace

// Converts each frame into the encoder's Y'CbCr planes on the frame's command buffer, reads them back, and once the GPU
// finished copies them into a picture of the encoder's pool for the writer.
struct Recorder::PlatformSession final : Recorder::Session {
    PlatformSession(Recorder& owner, Settings s, double fps, AudioPlan audio, SpaceProvider space,
                    std::chrono::milliseconds spaceInterval);
    ~PlatformSession() override;

    Done encode(gpu::CommandBuffer& cmd, const Frame& frame, const Composition& composition, double seconds) override;
    std::string pixels() const override { return nameOf(encoder->layout()); }
    void waitForEncoder() override;

    Slot* freeSlot();

    Recorder& owner;
    uint32_t width = 0;
    uint32_t height = 0;
    std::optional<uint64_t> screen;
    // The writer's, which outlives every frame handed to it.
    recording::FfmpegEncoder* encoder = nullptr;
    std::unique_ptr<YCbCrPass> pass;
    std::unique_ptr<ScreenPainter> painter;
    // What the painter paints for the conversion.
    gpu::Texture painted;
    std::array<Slot, kSlots> slots;
};

Recorder::PlatformSession::PlatformSession(Recorder& owner, Settings s, double fps, AudioPlan audio,
                                           SpaceProvider space, std::chrono::milliseconds spaceInterval)
    : owner(owner)
{
    Engine& engine = owner.engine_;
    screen = screenOf(s.target, *engine.composition());
    const uint32_t compositionWidth = engine.config().width;
    const uint32_t compositionHeight = engine.config().height;
    width = (s.width ? s.width : compositionWidth) & ~1u;
    height = (s.height ? s.height : compositionHeight) & ~1u;
    if (width < 16 || height < 16 || width > 8192 || height > 8192)
        throw std::runtime_error("recording size out of range");
    const bool whole = !screen && width == compositionWidth && height == compositionHeight;

    writer = std::make_unique<recording::Writer>(
        std::move(s), std::move(audio), std::move(space), spaceInterval,
        [this](const Ended& ended) { this->owner.ended(*this, ended); },
        [&owner] {
            if (owner.onSpace_)
                owner.onSpace_();
        });
    const Settings& settings = writer->settings;
    writer->open(
        [&](recording::MovieEncoder::Counts& counts) {
            auto made = std::make_unique<recording::FfmpegEncoder>(settings.codec, width, height, fps,
                                                                   settings.precise, counts);
            encoder = made.get();
            return made;
        },
        owner.encoderSetup_);

    const gpu::Device& device = engine.gpuDevice();
    const YCbCrFormat format = ycbcrFormat(encoder->layout());
    pass = std::make_unique<YCbCrPass>(device, format, averageCalibration(format));
    if (!whole) {
        painter = std::make_unique<ScreenPainter>(device, kFrameFormat,
                                                  std::array<uint32_t, 2>{compositionWidth, compositionHeight});
        painter->setGpuTimeline(&engine.gpuTimeline());
        painted = device.makeTexture({.format = kFrameFormat,
                                      .width = width,
                                      .height = height,
                                      .usage = gpu::Usage::RenderTarget | gpu::Usage::Sampled});
    }
    for (Slot& slot : slots) {
        slot.planes = pass->makePlanes(device, width, height);
        for (const gpu::Texture& plane : slot.planes)
            slot.readbacks.emplace_back(device, plane);
        if (slot.planes.empty() || (!whole && !painted))
            throw std::runtime_error("cannot allocate recording buffers");
    }
}

Recorder::PlatformSession::~PlatformSession()
{
    writer.reset();
}

Slot* Recorder::PlatformSession::freeSlot()
{
    for (Slot& slot : slots) {
        bool expected = false;
        if (slot.busy.compare_exchange_strong(expected, true))
            return &slot;
    }
    return nullptr;
}

FrameTap::Done Recorder::PlatformSession::encode(gpu::CommandBuffer& cmd, const Frame& frame,
                                                 const Composition& composition, double seconds)
{
    std::shared_ptr<YCbCrPicture> picture = encoder->newPicture();
    Slot* slot = picture ? freeSlot() : nullptr;
    if (!slot) {
        writer->drop();
        return nullptr;
    }
    const gpu::Texture* source = &frame.texture;
    if (painter) {
        painter->encode(cmd, &frame, recordedView(screen, composition), painted);
        source = &painted;
    }
    pass->encode(cmd, *source, slot->planes);
    for (const gpu::Readback& readback : slot->readbacks)
        readback.encode(cmd);
    writer->begin();
    return [self = shared_from_this(), this, slot, picture = std::move(picture), seconds](bool ok) mutable {
        if (ok)
            copyPlanes(pass->format(), slot->readbacks, *picture);
        slot->busy = false;
        writer->queued(std::move(picture), seconds, ok);
    };
}

// FFmpeg's encoders let go of a frame as soon as they encoded it; what is left is the GPU's.
void Recorder::PlatformSession::waitForEncoder()
{
    const auto idle = [this] {
        return std::none_of(slots.begin(), slots.end(), [](const Slot& slot) { return slot.busy.load(); });
    };
    for (int i = 0; i < 1000 && !writer->done && !idle(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

std::shared_ptr<Recorder::Session> Recorder::makeSession(Settings settings, AudioPlan audio, SpaceProvider space,
                                                         std::chrono::milliseconds spaceInterval)
{
    return std::make_shared<PlatformSession>(*this, std::move(settings), fps_, std::move(audio), std::move(space),
                                             spaceInterval);
}

bool Recorder::plays(const std::filesystem::path& file)
{
    return recording::moviePlays(file);
}

} // namespace azl
