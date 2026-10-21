#include "engine/settings_service.hpp"

#include <stdexcept>

namespace azl {

void applySettings(const MachineSettings& settings, Engine& engine, PreviewService& previews)
{
    engine.setGuardProfile(settings.profile, settings.protectTransitions);
    engine.setBakeStatic(settings.bakeStatic);
    engine.setRenderScale(int(settings.renderScale));
    engine.setCacheLimits(size_t(settings.frameCacheMB) << 20, size_t(settings.preloadMB) << 20);
    previews.setKeptClips(size_t(settings.clipPreviews));
}

Message settingsUse(Engine& engine, PreviewService& previews)
{
    const ClipDecoders::Use use = engine.cacheUse();
    const auto [kept, bytes] = previews.keptClipUse();
    return {{"frameCacheMB", use.playingBytes >> 20},
            {"preloadMB", use.preloadBytes >> 20},
            {"clipPreviews", kept},
            {"clipPreviewKB", bytes >> 10}};
}

SettingsService::SettingsService(DocumentServer& document, StartingSettings start, std::filesystem::path file,
                                 Apply apply, Use use)
    : document_(document), file_(std::move(file)), apply_(std::move(apply)), use_(std::move(use)),
      settings_(start.settings), overridden_(std::move(start.overridden))
{
    document_.handle("settings", [this](const Message& message, DocumentServer::Sink reply) { handle(message, reply); });
}

SettingsService::~SettingsService()
{
    document_.handle("settings", nullptr);
}

MachineSettings SettingsService::settings()
{
    std::lock_guard lock(mutex_);
    return settings_;
}

void SettingsService::overridden(const std::string& field, const std::string& flag)
{
    std::lock_guard lock(mutex_);
    overridden_[field] = flag;
}

void SettingsService::handle(const Message& message, const DocumentServer::Sink& reply)
{
    const std::string op = message.value("op", "");
    try {
        if (op == "state")
            return reply(state(op));
        if (op != "set")
            throw std::runtime_error("unknown settings op " + op);
        {
            std::lock_guard lock(mutex_);
            settings_ = applied(message, settings_);
            saveMachineSettings(file_, settings_);
            if (apply_)
                apply_(settings_);
        }
        reply(state(op));
        document_.broadcast(state("state"));
    } catch (const std::exception& e) {
        reply({{"type", "error"}, {"message", e.what()}});
    }
}

Message SettingsService::state(const std::string& op)
{
    std::lock_guard lock(mutex_);
    Message state = toJson(settings_);
    state["type"] = "settings";
    state["op"] = op;
    state["use"] = use_ ? use_() : Message::object();
    state["overridden"] = Message(overridden_);
    state["sections"] = Message::array({"audio", "displays", "performance", "caches"});
    return state;
}

} // namespace azl
