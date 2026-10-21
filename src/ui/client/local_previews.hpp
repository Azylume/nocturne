#pragma once

#include "ui/client/engine_client.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace azl {
class MediaLibrary;
}

namespace azl::ui {

// Renders a UI's previews on its own machine, so the engine it controls paints none for it. A follower
// engine in this process mirrors that engine's document and beat clock through Node Sync, never copying media, and
// serves previews through shared surfaces to client(). Which media this machine's library has is for the caller to
// tell; mediaAdded() makes clips of newly arrived media play at once.
class LocalPreviews {
public:
    struct Options {
        // This machine's Media Library; empty for the default one. Ignored when `shared` is given.
        std::filesystem::path library;
        // This machine's Media Library as the caller holds it; it must outlive the follower.
        MediaLibrary* shared = nullptr;
        // The engine's composition size, which effects and mapping are drawn at.
        uint32_t width = 1920;
        uint32_t height = 1080;
        double fps = 30.0;
        // The engine machine's pairing token, for an engine on another machine.
        std::string token;
    };

    // Follows the engine at host:port; `previewArrived` is called on any thread with every new picture. Throws when
    // this machine cannot render (no Metal device).
    LocalPreviews(const std::string& host, uint16_t port, std::function<void()> previewArrived, Options options);
    LocalPreviews(const std::string& host, uint16_t port, std::function<void()> previewArrived);
    ~LocalPreviews();

    EngineClient& client();
    // The media of `hash` arrived in this machine's library: clips of it that could not open try again.
    void mediaAdded(const std::string& hash);
    // Off, the follower renders nothing; it stays in sync meanwhile.
    void setRendering(bool on);
    bool rendering() const;
    void setClipPreviews(bool on);
    // Frames the follower has rendered.
    uint64_t frames() const;
    // The follower has the engine's document.
    bool synced() const;
    // Renders `copy` (Prepare's local copy) instead of the engine's document; null follows the engine again.
    void show(std::shared_ptr<const Composition> copy);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace azl::ui
