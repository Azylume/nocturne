#include "output/return_video_service.hpp"

#ifdef __APPLE__
#include <CoreVideo/CoreVideo.h>
#endif

#include <algorithm>

namespace azl {

namespace {

using Clock = std::chrono::steady_clock;

std::vector<std::pair<std::string, uint64_t>> outputsOf(const Composition& composition)
{
    std::vector<std::pair<std::string, uint64_t>> outputs;
    for (const Screen& screen : composition.screens)
        if (!screen.display.empty())
            outputs.emplace_back(screen.display, screen.id);
    return outputs;
}

const Screen* screenOf(const Composition& composition, uint64_t id)
{
    for (const Screen& screen : composition.screens)
        if (screen.id == id)
            return &screen;
    return nullptr;
}

uint64_t microsSince(Clock::time_point start)
{
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

} // namespace

ReturnVideoService::ReturnVideoService(DocumentServer& document, Engine& engine)
    : document_(document),
      engine_(engine),
      queue_(engine.gpuDevice().makeQueue()),
      painter_(engine.gpuDevice(), kFrameFormat, {engine.config().width, engine.config().height}),
#ifndef __APPLE__
      ycbcr_(engine.gpuDevice(), ycbcrFormat(YCbCrLayout::NV12), averageCalibration(ycbcrFormat(YCbCrLayout::NV12))),
#endif
      thread_([this] {
          AZL_THREAD_NAME("return video");
          run();
      })
{
    document_.handle("video", [this](const Message& message, DocumentServer::Sink reply) { handle(message, reply); });
}

ReturnVideoService::~ReturnVideoService()
{
    document_.handle("video", nullptr);
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

size_t ReturnVideoService::watchers() const
{
    std::lock_guard lock(mutex_);
    return watchers_.size();
}

double ReturnVideoService::fpsLocked() const
{
    double fps = return_video::kMinFps;
    for (const auto& [client, watcher] : watchers_)
        fps = std::max(fps, watcher.fps);
    return fps;
}

Message ReturnVideoService::watchMessage(bool on) const
{
    const auto outputs = outputsOf(*engine_.composition());
    const return_video::Plan plan =
        return_video::plan({engine_.config().width, engine_.config().height}, outputs.size(), fpsLocked());
    Message list = Message::array();
    for (const auto& [display, screen] : outputs)
        list.push_back({{"display", display}, {"screen", screen}});
    return {{"type", "video"},
            {"op", "watch"},
            {"on", on},
            {"fps", plan.fps},
            {"width", plan.width},
            {"height", plan.height},
            {"kbps", plan.kbps},
            {"budgetKbps", return_video::kBudgetKbps},
            {"outputs", list}};
}

void ReturnVideoService::handle(const Message& message, const DocumentServer::Sink& reply)
{
    const std::string op = message.value("op", "");
    const ClientId client = message.value("client", ClientId(0));
    if (op == "state") {
        std::lock_guard lock(mutex_);
        reply({{"type", "video"},
               {"op", "state"},
               {"watchers", watchers_.size()},
               {"outputs", outputsOf(*engine_.composition()).size()},
               {"pictures", stats_.pictures.load()},
               {"bytes", stats_.bytes.load()},
               {"hardware", hardware_ ? Message(*hardware_) : Message(nullptr)}});
        return;
    }
    if (op != "watch") {
        reply({{"type", "error"}, {"message", "unknown video op " + op}});
        return;
    }
    const bool on = message.value("on", true);
    {
        std::lock_guard lock(mutex_);
        const bool first = watchers_.empty();
        if (on) {
            watchers_[client].fps = std::clamp(message.value("fps", return_video::kFps), return_video::kMinFps,
                                               return_video::kMaxFps);
            newWatcher_ = true;
        } else {
            watchers_.erase(client);
        }
        const Message answer = watchMessage(on);
        if (on && first)
            announced_ = answer;
        reply(answer);
    }
    wake_.notify_all();
}

void ReturnVideoService::run()
{
    std::unique_lock lock(mutex_);
    Clock::time_point due = Clock::now();
    while (!stopping_) {
        if (watchers_.empty()) {
            outputs_.clear();
#ifdef __APPLE__
            textures_.clear();
#endif
            wake_.wait(lock);
            due = Clock::now();
            continue;
        }
        if (Clock::now() < due) {
            wake_.wait_until(lock, due);
            continue;
        }
        const double fps = fpsLocked();
        const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
        due = std::max(due + interval, Clock::now());
        const bool key = std::exchange(newWatcher_, false);
        std::vector<ClientId> watchers;
        for (const auto& [client, watcher] : watchers_)
            watchers.push_back(client);
        Message announce = watchMessage(true);
        if (announce == announced_)
            announce = nullptr;
        else
            announced_ = announce;
        lock.unlock();

        if (!announce.is_null())
            for (ClientId client : watchers)
                document_.send(client, announce);
        const std::shared_ptr<const Composition> composition = engine_.composition();
        const bool changed = follow(*composition, fps);
        const std::set<ClientId> gone = serve(engine_.latestFrame(), *composition, key || changed, watchers);

        lock.lock();
        for (ClientId client : gone)
            watchers_.erase(client);
    }
}

// Service thread: a new encoder for every output when the outputs, their size, rate or share of the budget change.
bool ReturnVideoService::follow(const Composition& composition, double fps)
{
    const auto outputs = outputsOf(composition);
    const return_video::Plan plan =
        return_video::plan({engine_.config().width, engine_.config().height}, outputs.size(), fps);
    bool same = plan.width == plan_.width && plan.fps == plan_.fps && plan.kbps == plan_.kbps &&
                outputs.size() == outputs_.size();
    for (size_t i = 0; same && i < outputs.size(); ++i)
        same = outputs_[i].display == outputs[i].first && outputs_[i].screen == outputs[i].second &&
               outputs_[i].encoder;
    if (same)
        return false;
    plan_ = plan;
#ifdef __APPLE__
    textures_.clear();
#endif
    std::vector<Output> next;
    for (const auto& [display, screen] : outputs) {
        Output output{.display = display, .screen = screen};
        try {
            output.encoder = std::make_unique<H264Encoder>(plan.width, plan.height, plan.fps, plan.kbps);
        } catch (const std::exception&) {
        }
#ifndef __APPLE__
        if (output.encoder && !makeTargets(output))
            output.encoder.reset();
#endif
        next.push_back(std::move(output));
    }
    outputs_ = std::move(next);
    std::lock_guard lock(mutex_);
    hardware_.reset();
    for (const Output& output : outputs_)
        if (output.encoder && output.encoder->hardware())
            hardware_ = hardware_.value_or(true) && *output.encoder->hardware();
    return true;
}

#ifdef __APPLE__
MTL::Texture* ReturnVideoService::textureOf(CVPixelBufferRef buffer)
{
    IOSurfaceRef surface = CVPixelBufferGetIOSurface(buffer);
    if (!surface)
        return nullptr;
    if (auto it = textures_.find(surface); it != textures_.end())
        return it->second.get();
    auto* desc = MTL::TextureDescriptor::texture2DDescriptor(
        gpu::metalFormat(kFrameFormat), CVPixelBufferGetWidth(buffer), CVPixelBufferGetHeight(buffer), false);
    desc->setUsage(MTL::TextureUsageRenderTarget);
    desc->setStorageMode(MTL::StorageModeShared);
    auto texture = NS::TransferPtr(engine_.device()->newTexture(desc, surface, 0));
    if (!texture) {
        desc->setStorageMode(MTL::StorageModeManaged);
        texture = NS::TransferPtr(engine_.device()->newTexture(desc, surface, 0));
    }
    if (!texture)
        return nullptr;
    return (textures_[surface] = texture).get();
}

// Straight into the encoder's own buffer.
std::optional<ReturnVideoService::Painted> ReturnVideoService::paint(gpu::CommandBuffer& cmd, Output& output,
                                                                     const Frame* frame,
                                                                     const Composition& composition,
                                                                     const Screen& screen)
{
    CVPixelBufferRef buffer = output.encoder->newBuffer();
    MTL::Texture* texture = buffer ? textureOf(buffer) : nullptr;
    if (!texture) {
        if (buffer)
            CVPixelBufferRelease(buffer);
        return std::nullopt;
    }
    const gpu::Texture target = gpu::Texture::retain(texture);
    painter_.encode(cmd, frame, screenView(composition, screen, false, std::nullopt), target);
    cmd.synchronize(target);
    return Painted{&output, buffer};
}

std::optional<return_video::Packet> ReturnVideoService::encode(const Painted& painted, double seconds, bool key)
{
    std::optional<return_video::Packet> packet = painted.output->encoder->encode(painted.buffer, seconds, key);
    CVPixelBufferRelease(painted.buffer);
    return packet;
}
#else
bool ReturnVideoService::makeTargets(Output& output)
{
    const gpu::Device& device = engine_.gpuDevice();
    output.painted = device.makeTexture({.format = kFrameFormat,
                                         .width = plan_.width,
                                         .height = plan_.height,
                                         .usage = gpu::Usage::RenderTarget | gpu::Usage::Sampled});
    output.planes = ycbcr_.makePlanes(device, plan_.width, plan_.height);
    output.readbacks.clear();
    for (const gpu::Texture& plane : output.planes)
        output.readbacks.emplace_back(device, plane);
    return output.painted && !output.planes.empty();
}

// Painted, converted to NV12 and read back; copied into the encoder's picture once the GPU finished.
std::optional<ReturnVideoService::Painted> ReturnVideoService::paint(gpu::CommandBuffer& cmd, Output& output,
                                                                     const Frame* frame,
                                                                     const Composition& composition,
                                                                     const Screen& screen)
{
    painter_.encode(cmd, frame, screenView(composition, screen, false, std::nullopt), output.painted);
    ycbcr_.encode(cmd, output.painted, output.planes);
    for (const gpu::Readback& readback : output.readbacks)
        readback.encode(cmd);
    return Painted{&output};
}

std::optional<return_video::Packet> ReturnVideoService::encode(const Painted& painted, double seconds, bool key)
{
    Output& output = *painted.output;
    const std::shared_ptr<YCbCrPicture> picture = output.encoder->newBuffer();
    if (!picture)
        return std::nullopt;
    copyPlanes(ycbcr_.format(), output.readbacks, *picture);
    return output.encoder->encode(*picture, seconds, key);
}
#endif

std::set<ClientId> ReturnVideoService::serve(const FrameRef& frame, const Composition& composition, bool key,
                                             const std::vector<ClientId>& watchers)
{
    std::vector<Painted> painted;
    {
        gpu::Scope scope;
        gpu::CommandBuffer cmd = queue_.commandBuffer();
        for (Output& output : outputs_) {
            const Screen* screen = screenOf(composition, output.screen);
            if (!screen || !output.encoder)
                continue;
            if (std::optional<Painted> p = paint(cmd, output, frame.get(), composition, *screen))
                painted.push_back(*p);
        }
        if (!painted.empty()) {
            cmd.commit();
            cmd.waitUntilCompleted();
            const gpu::Completion done = cmd.completion();
            stats_.gpuMicros += uint64_t(std::max(0.0, done.gpuEnd - done.gpuStart) * 1e6);
        }
    }

    std::set<ClientId> gone;
    const double seconds = std::chrono::duration<double>(Clock::now() - started_).count();
    for (const Painted& p : painted) {
        const Clock::time_point start = Clock::now();
        const std::optional<return_video::Packet> packet = encode(p, seconds, key);
        stats_.encodeMicros += microsSince(start);
        if (!packet)
            continue;
        ++stats_.pictures;
        const Message message = return_video::frameMessage(p.output->display, p.output->screen, p.output->frame++,
                                                           plan_, *packet);
        for (ClientId client : watchers) {
            if (document_.send(client, message))
                stats_.bytes += packet->data.size() + packet->config.size();
            else
                gone.insert(client);
        }
    }
    return gone;
}

} // namespace azl
