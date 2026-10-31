#include "output/return_video_service.hpp"

#include <CoreVideo/CoreVideo.h>

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
      queue_(NS::TransferPtr(engine.device()->newCommandQueue())),
      painter_(engine.device(), gpu::metalFormat(kFrameFormat), {engine.config().width, engine.config().height}),
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
            textures_.clear();
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
    textures_.clear();
    std::vector<Output> next;
    for (const auto& [display, screen] : outputs) {
        Output output{.display = display, .screen = screen};
        try {
            output.encoder = std::make_unique<H264Encoder>(plan.width, plan.height, plan.fps, plan.kbps);
        } catch (const std::exception&) {
        }
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

std::set<ClientId> ReturnVideoService::serve(const FrameRef& frame, const Composition& composition, bool key,
                                             const std::vector<ClientId>& watchers)
{
    struct Painted {
        Output* output;
        CVPixelBufferRef buffer;
    };
    std::vector<Painted> painted;
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    MTL::CommandBuffer* cmd = queue_->commandBuffer();
    for (Output& output : outputs_) {
        const Screen* screen = screenOf(composition, output.screen);
        CVPixelBufferRef buffer = screen && output.encoder ? output.encoder->newBuffer() : nullptr;
        MTL::Texture* texture = buffer ? textureOf(buffer) : nullptr;
        if (!texture) {
            if (buffer)
                CVPixelBufferRelease(buffer);
            continue;
        }
        painter_.encode(cmd, frame.get(), screenView(composition, *screen, false, std::nullopt), texture);
        if (texture->storageMode() == MTL::StorageModeManaged) {
            MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
            blit->synchronizeResource(texture);
            blit->endEncoding();
        }
        painted.push_back({&output, buffer});
    }
    if (!painted.empty()) {
        cmd->commit();
        cmd->waitUntilCompleted();
        stats_.gpuMicros += uint64_t(std::max(0.0, cmd->GPUEndTime() - cmd->GPUStartTime()) * 1e6);
    }
    pool->release();

    std::set<ClientId> gone;
    const double seconds = std::chrono::duration<double>(Clock::now() - started_).count();
    for (const Painted& p : painted) {
        const Clock::time_point start = Clock::now();
        const std::optional<return_video::Packet> packet = p.output->encoder->encode(p.buffer, seconds, key);
        CVPixelBufferRelease(p.buffer);
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
