#pragma once

#include "platform/autostart.hpp"
#include "protocol/document_server.hpp"
#include "supervisor/start_settings.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace azl {

// Where the Supervisor's state, its logs and the platform's autostart come from. Unset `state`: the engine runs without
// a supervisor.
struct ControlSource {
    // The Supervisor's status.json and machine-settings.json.
    std::filesystem::path state;
    // engine.log and supervisor.log.
    std::filesystem::path logs;
    // What this engine was started with.
    StartSettings running;
    // Default: the platform's (platform/autostart.hpp).
    std::function<AutostartState()> autostart = azl::autostartState;
    std::function<std::optional<std::string>(bool on)> setAutostart = azl::setAutostart;
    // Whether the engine lists an audio input called this; unset accepts any name.
    std::function<bool(const std::string& name)> audioInput;
};

// The Supervisor's status and the writes and reads around it, served by the engine (ADR 0017, phase 1):
//
//   status's "supervisor": null | {"running", "mode", "reason", "error", "enginePid", "startedAt", "now" (both Unix
//     seconds on this machine's clock, so a client computes the uptime), "starts", "crashes": [text], "autostart": "on"
//     | "off" | "unsupported"}, read from the Supervisor's status.json; null while the engine runs without a supervisor
//   {"op": "autostart", "on": bool} -> {"op": "autostart", "on"}: enables or disables the installed autostart job
//   {"op": "settings"[, "composition", "mediaRoot", "audioInput": text]} -> {"op": "settings", "saved": {the three},
//     "running": {the three}, "sources": {field: "saved" | "launch" | "default"}, "error": text | absent}: reads the
//     settings file the Supervisor applies at the next engine start, or first sets the fields given ("" clears one)
//   {"op": "logs", "file": "engine" | "supervisor"[, "lines": 1..200]} -> {"op": "logs", "file", "text", "truncated"}
//
// Autostart and settings writes are refused by `refusal` (the Show lock, a Gig window). Saved paths are confined as
// startSettingsRefusal says, whoever asks. No message names a file or a command. Every message has "type": "machine";
// errors are {"type": "error"}. Any thread; a call may run a system command and so take a moment.
class MachineControl {
public:
    using Refusal = std::function<std::optional<std::string>(const std::string& action)>;
    static constexpr auto kAutostartEvery = std::chrono::seconds(10);
    static constexpr size_t kDefaultLines = 100;

    MachineControl(ControlSource source, Refusal refusal);

    // Null without a supervisor.
    Message supervisor();
    // Handles "autostart", "settings" and "logs"; false for any other op.
    bool handle(const Message& message, const DocumentServer::Sink& reply);

private:
    void setAutostart(const Message& message, const DocumentServer::Sink& reply);
    void settings(const Message& message, const DocumentServer::Sink& reply);
    void logs(const Message& message, const DocumentServer::Sink& reply);
    AutostartState autostart();
    Message settingsReply(const StartSettings& saved, const std::optional<std::string>& error) const;

    ControlSource source_;
    Refusal refusal_;
    std::mutex mutex_;
    std::optional<std::chrono::steady_clock::time_point> autostartAt_;
    AutostartState autostart_ = AutostartState::Unsupported;
};

} // namespace azl
