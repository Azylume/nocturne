#include "ui/client/local_previews.hpp"

#include <stdexcept>

namespace azl::ui {

// Platforms whose engine does not render yet: the follower cannot be made, so the engine paints the previews.
struct LocalPreviews::Impl {};

LocalPreviews::LocalPreviews(const std::string&, uint16_t, std::function<void()>, Options)
{
    throw std::runtime_error("this platform's engine cannot render previews here yet");
}

LocalPreviews::LocalPreviews(const std::string& host, uint16_t port, std::function<void()> previewArrived)
    : LocalPreviews(host, port, std::move(previewArrived), Options{})
{
}

LocalPreviews::~LocalPreviews() = default;

// Never reached: no LocalPreviews exists.
EngineClient& LocalPreviews::client()
{
    throw std::logic_error("no local previews");
}

void LocalPreviews::mediaAdded(const std::string&) {}
void LocalPreviews::setRendering(bool) {}
bool LocalPreviews::rendering() const { return false; }
void LocalPreviews::setClipPreviews(bool) {}
uint64_t LocalPreviews::frames() const { return 0; }
bool LocalPreviews::synced() const { return false; }
void LocalPreviews::show(std::shared_ptr<const Composition>) {}

} // namespace azl::ui
