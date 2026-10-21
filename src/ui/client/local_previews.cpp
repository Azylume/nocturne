#include "ui/client/local_previews.hpp"

#include "engine/engine.hpp"
#include "engine/preview_service.hpp"
#include "engine/render_thread.hpp"
#include "media/media_library.hpp"
#include "protocol/document_server.hpp"
#include "sync/node_sync.hpp"
#include "sync/udp_clock.hpp"

#include <dispatch/dispatch.h>

#include <mutex>
#include <stdexcept>

namespace azl::ui {

namespace {

constexpr size_t kClipCacheBytes = size_t(256) << 20;
constexpr size_t kPreloadBytes = size_t(256) << 20;

NS::SharedPtr<MTL::Device> systemDevice()
{
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    if (!device)
        throw std::runtime_error("no Metal device");
    return device;
}

struct Forward : EngineClient::Listener {
    std::function<void()> arrived;
    void previewArrived(const std::string&) override { arrived(); }
};

// What the client sends reaches the document off the server's lock, in order: the server answers under its lock, and
// the client may send while it handles an answer.
struct Delivery {
    DocumentServer* document;
    ClientId client;
    Message message;
};

void deliver(void* context)
{
    std::unique_ptr<Delivery> delivery(static_cast<Delivery*>(context));
    delivery->document->receive(delivery->client, delivery->message);
}

} // namespace

struct LocalPreviews::Impl {
    Impl(const std::string& host, uint16_t port, std::function<void()> arrived, const Options& options)
        : device(systemDevice()),
          owned(options.shared ? nullptr
                               : std::make_unique<MediaLibrary>(options.library.empty() ? MediaLibrary::defaultRoot()
                                                                                        : options.library)),
          library(options.shared ? *options.shared : *owned),
          engine(device.get(),
                 {.width = options.width,
                  .height = options.height,
                  .clipCacheBytes = kClipCacheBytes,
                  .preloadBytes = kPreloadBytes},
                 &library),
          document(defaultComposition(), [this](auto composition) { follow(std::move(composition)); }),
          previews(document, engine), fps(options.fps), client(forward),
          sends(dispatch_queue_create("nocturne.ui.local-previews", DISPATCH_QUEUE_SERIAL))
    {
        forward.arrived = std::move(arrived);
        previews.setClipLibrary(&library);
        node = std::make_unique<NodeSync>(
            document, engine, library, engineTime,
            [host, port, token = options.token]() -> std::optional<Endpoint> {
                return Endpoint{host, host, port, token};
            },
            [](const Endpoint& main, uint16_t clockPort) -> std::unique_ptr<ClockLink> {
                return std::make_unique<ClockPinger>(main.host, clockPort, engineTime);
            },
            false);
        id = document.connect([this](const Message& message) { client.receive(message); });
        client.attached(
            [this](const Message& message) {
                dispatch_async_f(sends, new Delivery{&document, id, message}, deliver);
            },
            true);
        render = std::make_unique<RenderThread>(engine, fps);
    }

    ~Impl()
    {
        render.reset();
        node.reset();
        client.detached();
        document.disconnect(id);
        dispatch_sync_f(sends, nullptr, [](void*) {});
        dispatch_release(sends);
        engine.waitForCompiles();
    }

    // The engine's document reaches the follower engine unless a copy is shown.
    void follow(std::shared_ptr<const Composition> composition)
    {
        std::lock_guard lock(shownMutex);
        if (!shown)
            engine.setComposition(std::move(composition));
    }

    NS::SharedPtr<MTL::Device> device;
    std::unique_ptr<MediaLibrary> owned;
    MediaLibrary& library;
    Engine engine;
    std::mutex shownMutex;
    std::shared_ptr<const Composition> shown;
    DocumentServer document;
    PreviewService previews;
    double fps;
    Forward forward;
    EngineClient client;
    dispatch_queue_t sends;
    ClientId id = 0;
    std::unique_ptr<NodeSync> node;
    std::unique_ptr<RenderThread> render;
    std::mutex mutex;
};

LocalPreviews::LocalPreviews(const std::string& host, uint16_t port, std::function<void()> previewArrived,
                             Options options)
    : impl_(std::make_unique<Impl>(host, port, std::move(previewArrived), options))
{
}

LocalPreviews::LocalPreviews(const std::string& host, uint16_t port, std::function<void()> previewArrived)
    : LocalPreviews(host, port, std::move(previewArrived), Options{})
{
}

LocalPreviews::~LocalPreviews() = default;

EngineClient& LocalPreviews::client()
{
    return impl_->client;
}

void LocalPreviews::mediaAdded(const std::string& hash)
{
    impl_->engine.mediaAdded(hash);
}

void LocalPreviews::setRendering(bool on)
{
    std::lock_guard lock(impl_->mutex);
    if (on == bool(impl_->render))
        return;
    if (on)
        impl_->render = std::make_unique<RenderThread>(impl_->engine, impl_->fps);
    else
        impl_->render.reset();
}

bool LocalPreviews::rendering() const
{
    std::lock_guard lock(impl_->mutex);
    return bool(impl_->render);
}

void LocalPreviews::setClipPreviews(bool on)
{
    if (on != impl_->previews.clipPreviews())
        impl_->previews.setClipPreviews(on);
}

uint64_t LocalPreviews::frames() const
{
    return impl_->engine.stats().rendered.load();
}

void LocalPreviews::show(std::shared_ptr<const Composition> copy)
{
    {
        std::lock_guard lock(impl_->shownMutex);
        if (!copy && !impl_->shown)
            return;
        if (copy) {
            impl_->shown = copy;
            impl_->engine.setComposition(std::move(copy));
            return;
        }
    }
    // Taken outside shownMutex: the document's lock is held while it publishes into follow().
    auto followed = std::make_shared<const Composition>(impl_->document.document());
    std::lock_guard lock(impl_->shownMutex);
    impl_->shown.reset();
    impl_->engine.setComposition(std::move(followed));
}

bool LocalPreviews::synced() const
{
    return impl_->node->status().synced;
}

} // namespace azl::ui
