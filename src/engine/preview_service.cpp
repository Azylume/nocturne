#include "engine/preview_service.hpp"

#include "platform/thread_priority.hpp"
#include "platform/profile.hpp"
#include "media/stills.hpp"
#include "protocol/preview_surface.hpp"

#include <IOSurface/IOSurface.h>

#include <algorithm>
#include <cstring>
#include <set>

namespace azl {

namespace {

constexpr size_t kSurfaceRing = 3;

using Clock = std::chrono::steady_clock;

std::optional<uint64_t> idAfter(const std::string& source, const std::string& prefix)
{
    if (source.rfind(prefix, 0) != 0)
        return std::nullopt;
    try {
        return std::stoull(source.substr(prefix.size()));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool hasLayer(const Composition& composition, uint64_t id)
{
    bool found = false;
    forEachLayer(composition.layers, [&](const Layer& layer) { found = found || layer.id == id; });
    return found;
}

std::string sourceKind(const std::string& source)
{
    return source.substr(0, source.find(':'));
}

std::string stripKey(const std::string& hash, uint32_t width, uint32_t height)
{
    return hash + "@" + std::to_string(width) + "x" + std::to_string(height);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
// Global, so the UI process finds it by id. The deprecated flag still works for unsandboxed processes; passing a mach
// port instead needs a channel the protocol does not have.
IOSurfaceRef globalSurface(uint32_t width, uint32_t height)
{
    CFMutableDictionaryRef props =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    auto number = [&](CFStringRef key, int value) {
        CFNumberRef n = CFNumberCreate(nullptr, kCFNumberIntType, &value);
        CFDictionarySetValue(props, key, n);
        CFRelease(n);
    };
    number(kIOSurfaceWidth, int(width));
    number(kIOSurfaceHeight, int(height));
    number(kIOSurfaceBytesPerElement, 4);
    number(kIOSurfacePixelFormat, int('BGRA'));
    CFDictionarySetValue(props, kIOSurfaceIsGlobal, kCFBooleanTrue);
    IOSurfaceRef surface = IOSurfaceCreate(props);
    CFRelease(props);
    return surface;
}
#pragma clang diagnostic pop

void markSurface(IOSurfaceRef surface, int64_t frame)
{
    CFNumberRef value = CFNumberCreate(nullptr, kCFNumberSInt64Type, &frame);
    IOSurfaceSetValue(surface, previewFrameKey(), value);
    CFRelease(value);
}

void copyInto(IOSurfaceRef surface, const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height)
{
    IOSurfaceLock(surface, 0, nullptr);
    auto* base = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(surface));
    const size_t stride = IOSurfaceGetBytesPerRow(surface);
    const size_t row = size_t(width) * 4;
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(base + y * stride, bgra.data() + y * row, row);
    IOSurfaceUnlock(surface, 0, nullptr);
}

} // namespace

std::optional<std::string> clipHash(const std::string& source)
{
    if (source.rfind("clip:", 0) != 0 || source.size() == 5)
        return std::nullopt;
    return source.substr(5);
}

bool previewScreen(const std::string& source, const Composition& composition, Screen& screen)
{
    if (source == "composition") {
        screen = Screen{.slices = {Slice{}}};
        return true;
    }
    if (auto layer = idAfter(source, "layer:"); layer && hasLayer(composition, *layer)) {
        screen = Screen{.slices = {Slice{.input = *layer}}};
        return true;
    }
    if (auto id = idAfter(source, "screen:")) {
        for (const Screen& s : composition.screens)
            if (s.id == *id) {
                screen = s;
                return true;
            }
    }
    return false;
}

PreviewService::PreviewService(DocumentServer& document, Engine& engine)
    : document_(document),
      engine_(engine),
      queue_(NS::TransferPtr(engine.device()->newCommandQueue())),
      gpu_(engine.device(), "previews", ""),
      painter_(engine.device(), kFrameFormat, {engine.config().width, engine.config().height}),
      thread_([this] {
          AZL_THREAD_NAME("previews");
          run();
      }),
      sampler_([this] { sample(); })
{
    painter_.setGpuTimeline(&gpu_);
    document_.handle("preview", [this](const Message& message, DocumentServer::Sink reply) { handle(message, reply); });
}

PreviewService::~PreviewService()
{
    document_.handle("preview", nullptr);
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    sampleWake_.notify_all();
    thread_.join();
    sampler_.join();
    engine_.setPreviewPictures({});
}

void PreviewService::setClipLibrary(MediaLibrary* library)
{
    if (library && !effects_)
        effects_ = std::make_unique<ClipEffectsPreview>(engine_.device());
    library_ = library;
}

void PreviewService::setClipPreviews(bool on)
{
    {
        std::lock_guard lock(mutex_);
        if (on && library_ && !clipsOn_) {
            clipsOn_ = true;
            for (auto& [key, watch] : watches_)
                addClipTargets(watch);
        } else if (!on && clipsOn_) {
            clipsOn_ = false;
            for (auto& [key, watch] : watches_)
                std::erase_if(watch.targets, [](const Target& target) { return !target.strip.empty(); });
            strips_.clear();
            if (effects_)
                effects_->release();
        }
        updateStrips();
    }
    wake_.notify_all();
    sampleWake_.notify_all();
    document_.broadcast(clipsState());
}

Message PreviewService::clipsState() const
{
    return {{"type", "preview"}, {"op", "clips"}, {"on", clipsOn_.load()}};
}

// Under mutex_.
void PreviewService::addClipTargets(Watch& watch) const
{
    for (const std::string& source : watch.clipSources)
        watch.targets.push_back(makeTarget(source, watch));
}

void PreviewService::handle(const Message& message, const DocumentServer::Sink& reply)
{
    if (message.value("op", "") == "clips") {
        auto on = message.find("on");
        if (on == message.end() || !on->is_boolean())
            reply(clipsState());
        else if (!library_)
            reply({{"type", "error"}, {"message", "clip previews need a media library"}});
        else
            setClipPreviews(on->get<bool>());
        return;
    }
    if (message.value("op", "") != "watch") {
        reply({{"type", "error"}, {"message", "unknown preview op " + message.value("op", "")}});
        return;
    }
    const WatchKey key{message.value("client", ClientId(0)), message.value("watch", "")};
    Watch watch;
    watch.surface = message.value("via", "jpeg") == "surface";
    watch.width = std::clamp(message.value("width", 320u), 16u, 1920u);
    watch.height = std::clamp(message.value("height", 180u), 16u, 1080u);
    watch.interval = 1.0 / std::clamp(message.value("fps", 30.0), 1.0, 60.0);
    if (auto clip = message.find("clip"); clip != message.end() && clip->is_object())
        watch.cell = Cell{clip->value("layer", uint64_t(0)), clip->value("column", uint64_t(0))};
    std::set<std::string> seen;
    if (auto sources = message.find("sources"); sources != message.end() && sources->is_array())
        for (const auto& source : *sources) {
            if (!source.is_string() || !seen.insert(source.get<std::string>()).second)
                continue;
            if (!clipHash(source.get<std::string>()))
                watch.targets.push_back(makeTarget(source.get<std::string>(), watch));
            else if (watch.clipSources.size() < kMaxClips)
                watch.clipSources.push_back(source.get<std::string>());
        }
    {
        std::lock_guard lock(mutex_);
        if (clipsOn_)
            addClipTargets(watch);
        if (watch.targets.empty() && watch.clipSources.empty())
            watches_.erase(key);
        else
            watches_[key] = std::move(watch);
        updatePictures();
        updateStrips();
    }
    wake_.notify_all();
    sampleWake_.notify_all();
}

PreviewService::Target PreviewService::makeTarget(const std::string& source, const Watch& watch) const
{
    Target target;
    target.source = source;
    const std::optional<std::string> hash = clipHash(source);
    if (hash) {
        target.strip = stripKey(*hash, watch.width, watch.height);
        target.cell = watch.cell;
    }
    MTL::Device* device = engine_.device();
    auto* desc = MTL::TextureDescriptor::texture2DDescriptor(kFrameFormat, watch.width, watch.height, false);
    desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    desc->setStorageMode(device->hasUnifiedMemory() ? MTL::StorageModeShared : MTL::StorageModeManaged);
    for (size_t i = 0; i < (watch.surface ? kSurfaceRing : 1); ++i) {
        if (!watch.surface) {
            if (!hash)
                target.textures.push_back(NS::TransferPtr(device->newTexture(desc)));
            continue;
        }
        IOSurfaceRef surface = globalSurface(watch.width, watch.height);
        if (!surface)
            break;
        target.surfaces.emplace_back(surface, CFRelease);
        if (!hash)
            target.textures.push_back(NS::TransferPtr(device->newTexture(desc, surface, 0)));
    }
    return target;
}

// Under mutex_.
void PreviewService::updatePictures()
{
    std::set<uint64_t> layers;
    for (const auto& [key, watch] : watches_)
        for (const Target& target : watch.targets)
            if (auto id = idAfter(target.source, "layer:"))
                layers.insert(*id);
    engine_.setPreviewPictures(std::move(layers));
}

// Under mutex_. Every watched clip has a strip; of the others, the most recently watched kKeptClips stay.
void PreviewService::updateStrips()
{
    ++watchGeneration_;
    for (auto& [key, strip] : strips_)
        strip.watched = false;
    for (const auto& [key, watch] : watches_)
        for (const Target& target : watch.targets) {
            if (target.strip.empty())
                continue;
            Strip& strip = strips_[target.strip];
            strip.hash = *clipHash(target.source);
            strip.width = watch.width;
            strip.height = watch.height;
            strip.watched = true;
            strip.lastWatched = watchGeneration_;
        }
    std::vector<std::pair<uint64_t, std::string>> idle;
    for (const auto& [key, strip] : strips_)
        if (!strip.watched)
            idle.emplace_back(strip.lastWatched, key);
    std::sort(idle.begin(), idle.end());
    for (size_t i = 0; i + keptClips_ < idle.size(); ++i)
        strips_.erase(idle[i].second);
}

void PreviewService::setKeptClips(size_t kept)
{
    std::lock_guard lock(mutex_);
    keptClips_ = kept;
    updateStrips();
}

std::pair<size_t, size_t> PreviewService::keptClipUse()
{
    std::lock_guard lock(mutex_);
    size_t unwatched = 0, bytes = 0;
    for (const auto& [key, strip] : strips_) {
        unwatched += strip.watched ? 0 : 1;
        for (const auto& picture : strip.pictures)
            bytes += picture.size();
        for (const auto& jpeg : strip.jpegs)
            bytes += jpeg.size();
    }
    return {unwatched, bytes};
}

std::map<std::string, size_t> PreviewService::sizes()
{
    std::lock_guard lock(mutex_);
    std::map<std::string, size_t> sizes = effects_ ? effects_->sizes() : std::map<std::string, size_t>{};
    size_t targets = 0, surfaces = 0, surfaceBytes = 0, textures = 0, textureBytes = 0;
    for (const auto& [key, watch] : watches_)
        for (const Target& target : watch.targets) {
            ++targets;
            surfaces += target.surfaces.size();
            for (const auto& surface : target.surfaces)
                surfaceBytes += IOSurfaceGetAllocSize(surface.get());
            textures += target.textures.size();
            for (const auto& texture : target.textures)
                if (!texture->iosurface())
                    textureBytes += texture->allocatedSize();
        }
    size_t pictures = 0, pictureBytes = 0, jpegBytes = 0;
    for (const auto& [key, strip] : strips_) {
        pictures += strip.pictures.size();
        for (const auto& picture : strip.pictures)
            pictureBytes += picture.size();
        for (const auto& jpeg : strip.jpegs)
            jpegBytes += jpeg.size();
    }
    sizes.insert({{"previewWatches", watches_.size()}, {"previewTargets", targets},
                  {"previewSurfaces", surfaces}, {"previewSurfaceKB", surfaceBytes >> 10},
                  {"previewTextures", textures}, {"previewTextureKB", textureBytes >> 10},
                  {"clipStrips", strips_.size()}, {"clipPictures", pictures},
                  {"clipPictureKB", pictureBytes >> 10}, {"clipJpegKB", jpegBytes >> 10}});
    return sizes;
}

std::map<std::string, size_t> PreviewService::watchers()
{
    std::lock_guard lock(mutex_);
    std::map<std::string, size_t> watchers;
    for (const auto& [key, watch] : watches_) {
        std::set<std::string> kinds;
        for (const Target& target : watch.targets)
            kinds.insert(sourceKind(target.source));
        for (const std::string& kind : kinds)
            ++watchers[kind];
    }
    return watchers;
}

void PreviewService::dropClient(ClientId client)
{
    std::erase_if(watches_, [&](const auto& watch) { return watch.first.first == client; });
}

void PreviewService::run()
{
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        if (watches_.empty()) {
            wake_.wait(lock);
            continue;
        }
        Clock::time_point due = Clock::time_point::max();
        for (const auto& [key, watch] : watches_)
            due = std::min(due, watch.due);
        if (Clock::now() < due) {
            wake_.wait_until(lock, due);
            continue;
        }
        const FrameRef frame = engine_.latestFrame();
        const std::shared_ptr<const Composition> composition = engine_.composition();
        const Clock::time_point now = Clock::now();
        std::set<ClientId> gone;
        for (auto& [key, watch] : watches_) {
            if (watch.due > now)
                continue;
            watch.due = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(watch.interval));
            if (watch.targets.empty())
                continue;
            const bool fresh = frame && frame->index != watch.frame;
            if (fresh)
                watch.frame = frame->index;
            if (!serve(key.first, watch, fresh ? frame : FrameRef{}, composition))
                gone.insert(key.first);
        }
        for (ClientId client : gone)
            dropClient(client);
        if (!gone.empty()) {
            updatePictures();
            updateStrips();
        }
    }
}

bool PreviewService::serve(ClientId client, Watch& watch, const FrameRef& frame,
                           const std::shared_ptr<const Composition>& composition)
{
    bool connected = true;
    for (Target& target : watch.targets)
        if (!target.strip.empty())
            connected = serveClip(client, watch, target, composition) && connected;
    if (!frame)
        return connected;

    AZL_ZONE("paint previews");
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    MTL::CommandBuffer* cmd = queue_->commandBuffer();
    std::vector<Target*> painted;
    for (Target& target : watch.targets) {
        Screen screen;
        if (target.textures.empty() || !previewScreen(target.source, *composition, screen))
            continue;
        MTL::Texture* texture = target.textures[target.next].get();
        if (watch.surface)
            markSurface(target.surfaces[target.next].get(), -1);
        painter_.encode(cmd, frame.get(), ScreenView{screen}, texture);
        if (texture->storageMode() == MTL::StorageModeManaged) {
            MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
            blit->synchronizeResource(texture);
            blit->endEncoding();
        }
        painted.push_back(&target);
    }
    if (painted.empty()) {
        pool->release();
        return connected;
    }
    cmd->commit();
    cmd->waitUntilCompleted();
    AZL_GPU_COLLECT(gpu_);
    paintStats_.pictures += painted.size();
    paintStats_.gpuMicros += uint64_t(std::max(0.0, cmd->GPUEndTime() - cmd->GPUStartTime()) * 1e6);
    pool->release();

    for (Target* target : painted) {
        Message message = {{"type", "preview"}, {"op", "frame"}, {"source", target->source}, {"frame", frame->index}};
        if (watch.surface) {
            if (!target->announced) {
                Message ids = Message::array();
                for (const auto& surface : target->surfaces)
                    ids.push_back(IOSurfaceGetID(surface.get()));
                connected = document_.send(client, {{"type", "preview"},
                                                    {"op", "surfaces"},
                                                    {"source", target->source},
                                                    {"width", watch.width},
                                                    {"height", watch.height},
                                                    {"surfaces", ids}}) &&
                            connected;
                target->announced = true;
            }
            markSurface(target->surfaces[target->next].get(), int64_t(frame->index));
            message["surface"] = target->next;
        } else {
            const size_t bytesPerRow = size_t(watch.width) * 4;
            std::vector<uint8_t> pixels(bytesPerRow * watch.height);
            target->textures[target->next]->getBytes(pixels.data(), bytesPerRow,
                                                     MTL::Region(0, 0, watch.width, watch.height), 0);
            message["width"] = watch.width;
            message["height"] = watch.height;
            message["jpeg"] = Message::binary(encodeJpeg(pixels.data(), watch.width, watch.height, bytesPerRow));
        }
        connected = document_.send(client, message) && connected;
        target->next = (target->next + 1) % target->textures.size();
    }
    return connected;
}

// Under mutex_. Sends the strip's next picture, through the cell's clip effects when it has them; nothing while it has
// none, or only one already sent without effects.
bool PreviewService::serveClip(ClientId client, Watch& watch, Target& target,
                               const std::shared_ptr<const Composition>& composition)
{
    auto it = strips_.find(target.strip);
    if (it == strips_.end() || it->second.pictures.empty())
        return true;
    Strip& strip = it->second;
    const size_t count = strip.pictures.size();
    const size_t index = target.sent % count;
    std::vector<uint8_t> drawn;
    const bool effects = drawEffects(target, strip, index, composition, drawn);
    if (count == 1 && target.sent > 0 && !effects)
        return true;
    const std::vector<uint8_t>& picture = effects ? drawn : strip.pictures[index];
    bool connected = true;
    Message message = {{"type", "preview"}, {"op", "frame"}, {"source", target.source}, {"frame", target.sent}};
    if (watch.surface) {
        if (target.surfaces.empty())
            return true;
        if (!target.announced) {
            Message ids = Message::array();
            for (const auto& surface : target.surfaces)
                ids.push_back(IOSurfaceGetID(surface.get()));
            connected = document_.send(client, {{"type", "preview"},
                                                {"op", "surfaces"},
                                                {"source", target.source},
                                                {"width", watch.width},
                                                {"height", watch.height},
                                                {"surfaces", ids}});
            target.announced = true;
        }
        IOSurfaceRef surface = target.surfaces[target.next].get();
        markSurface(surface, -1);
        copyInto(surface, picture, strip.width, strip.height);
        markSurface(surface, int64_t(target.sent));
        message["surface"] = target.next;
        target.next = (target.next + 1) % target.surfaces.size();
    } else {
        strip.jpegs.resize(count);
        const size_t row = size_t(strip.width) * 4;
        if (!effects && strip.jpegs[index].empty())
            strip.jpegs[index] = encodeJpeg(picture.data(), strip.width, strip.height, row);
        message["width"] = strip.width;
        message["height"] = strip.height;
        message["jpeg"] =
            Message::binary(effects ? encodeJpeg(picture.data(), strip.width, strip.height, row) : strip.jpegs[index]);
    }
    ++target.sent;
    return document_.send(client, message) && connected;
}

bool PreviewService::drawEffects(const Target& target, const Strip& strip, size_t index,
                                 const std::shared_ptr<const Composition>& composition, std::vector<uint8_t>& picture)
{
    if (!target.cell || !effects_ || !composition)
        return false;
    const ClipRef* clip = nullptr;
    forEachLayer(composition->layers, [&](const Layer& layer) {
        if (layer.id != target.cell->layer)
            return;
        auto cell = layer.clips.find(target.cell->column);
        if (cell != layer.clips.end() && cell->second.hash == strip.hash && !cell->second.effects.empty())
            clip = &cell->second;
    });
    if (!clip)
        return false;
    picture = strip.pictures[index];
    const TransportState transport = engine_.transport();
    const std::string slot = std::to_string(target.cell->layer) + "/" + std::to_string(target.cell->column);
    return effects_->draw(queue_.get(), composition, *clip, slot, picture, strip.width, strip.height, transport.time,
                          transport.beat, engine_.audio());
}

// Samples the watched strips breadth first (every clip's first picture, then every second one), one picture at a
// time, resting three times as long as each took. As a background thread its rests also stretch with timer coalescing,
// and it yields to frames and decoding whenever the CPU is short.
void PreviewService::sample()
{
    setThreadRole(ThreadRole::Background);
    std::map<std::string, std::unique_ptr<VideoSampler>> samplers;
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        std::erase_if(samplers, [&](const auto& open) {
            auto it = strips_.find(open.first);
            return it == strips_.end() || !it->second.watched || it->second.failed ||
                   it->second.tried >= kClipPictures;
        });
        const std::pair<const std::string, Strip>* next = nullptr;
        for (const auto& entry : strips_) {
            const Strip& strip = entry.second;
            if (strip.watched && !strip.failed && strip.tried < kClipPictures && (!next || strip.tried < next->second.tried))
                next = &entry;
        }
        if (!next) {
            sampleWake_.wait(lock);
            continue;
        }
        const std::string key = next->first;
        const Strip job = {.hash = next->second.hash, .width = next->second.width, .height = next->second.height,
                           .tried = next->second.tried};
        lock.unlock();

        const Clock::time_point start = Clock::now();
        std::vector<uint8_t> picture;
        bool failed = false;
        try {
            std::unique_ptr<VideoSampler>& sampler = samplers[key];
            if (!sampler) {
                MediaLibrary* library = library_;
                const std::optional<MediaEntry> entry = library ? library->find(job.hash) : std::nullopt;
                if (!entry || entry->status != MediaStatus::Ok)
                    throw std::runtime_error("no playable media");
                sampler = std::make_unique<VideoSampler>(entry->path, job.width, job.height, kClipPictures);
            }
            picture = sampler->picture(job.tried);
        } catch (const std::exception&) {
            failed = true;
            samplers.erase(key);
        }
        const auto busy = Clock::now() - start;
        clipStats_.busyMicros += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(busy).count());
        if (!picture.empty())
            ++clipStats_.pictures;

        lock.lock();
        if (auto it = strips_.find(key); it != strips_.end() && it->second.tried == job.tried) {
            ++it->second.tried;
            it->second.failed = failed;
            if (!picture.empty())
                it->second.pictures.push_back(std::move(picture));
        }
        const auto rest = std::chrono::duration_cast<Clock::duration>(busy * (1.0 / kClipBusyShare - 1.0));
        sampleWake_.wait_for(lock, rest, [&] { return stopping_; });
    }
}

} // namespace azl
