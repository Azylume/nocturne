#pragma once

#include "engine/engine.hpp"
#include "engine/machine_settings.hpp"
#include "engine/preview_service.hpp"
#include "protocol/document_server.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace azl {

// Serves the machine's performance and cache settings as "settings" messages: machine-local, live, outside the
// document and its undo history.
//
//   {"type": "settings", "op": "state"}
//   {"type": "settings", "op": "set", "profile"?: "smooth" | "sharp", "protectTransitions"?, "frameCacheMB"?,
//    "preloadMB"?, "clipPreviews"?, "bakeStatic"?, "renderScale"?: 100 | 83 | 67}
//       applies the fields at once and keeps them in `file`, the state folder's; the next start takes them back
// Each is answered to its sender with its own op and the state; set also broadcasts it as "state":
//   {"type": "settings", "op": ..., "profile", "protectTransitions", "frameCacheMB", "preloadMB", "clipPreviews",
//    "bakeStatic", "renderScale",
//    "use": {"frameCacheMB", "preloadMB", "clipPreviews", "clipPreviewKB"}, "overridden": {"<field>": "<flag>"},
//    "sections": ["audio", "displays", "performance", "caches"]}
// "use" is what the caches hold now; "overridden" names the fields a command-line flag set on this start, which it
// sets again on the next ("audioInput" when the flags' audio input opened instead of the one picked); "sections" are
// the Settings sections this engine serves.
// Applies a machine's settings to its engine and previews: the guard profile, the cache bounds, the kept previews.
void applySettings(const MachineSettings& settings, Engine& engine, PreviewService& previews);
// What the caches hold now, for the "use" field.
Message settingsUse(Engine& engine, PreviewService& previews);

// `apply` runs on each set; the start's settings are the owner's to apply.
class SettingsService {
public:
    using Apply = std::function<void(const MachineSettings&)>;
    using Use = std::function<Message()>;

    SettingsService(DocumentServer& document, StartingSettings start, std::filesystem::path file, Apply apply = {},
                    Use use = {});
    ~SettingsService();

    MachineSettings settings();
    // Names a field a flag set on this start, as for the audio input, which its own service keeps.
    void overridden(const std::string& field, const std::string& flag);

private:
    void handle(const Message& message, const DocumentServer::Sink& reply);
    Message state(const std::string& op);

    DocumentServer& document_;
    const std::filesystem::path file_;
    Apply apply_;
    Use use_;
    std::mutex mutex_;
    MachineSettings settings_;
    std::map<std::string, std::string> overridden_;
};

} // namespace azl
