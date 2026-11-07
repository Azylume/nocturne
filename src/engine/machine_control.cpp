#include "engine/machine_control.hpp"

#include "document/composition_file.hpp"
#include "platform/process.hpp"
#include "supervisor/log_tail.hpp"

#include <algorithm>

namespace azl {

namespace {

Message error(const std::string& text)
{
    return {{"type", "error"}, {"message", text}};
}

Message toJson(const StartSettings& s)
{
    return {{"composition", s.composition}, {"mediaRoot", s.mediaRoot}, {"audioInput", s.audioInput}};
}

// The status.json the Supervisor keeps; an empty object when it is missing or unreadable.
Message readStatus(const std::filesystem::path& state)
{
    try {
        if (const std::optional<Message> j = readJsonFile(state / "status.json"); j && j->is_object())
            return *j;
    } catch (const std::exception&) {
    }
    return Message::object();
}

Message textOrNull(const Message& j, const char* key)
{
    const Message value = j.value(key, Message());
    return value.is_string() && !value.get<std::string>().empty() ? value : Message(nullptr);
}

}

MachineControl::MachineControl(ControlSource source, Refusal refusal)
    : source_(std::move(source)), refusal_(std::move(refusal))
{
}

AutostartState MachineControl::autostart()
{
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (!autostartAt_ || now - *autostartAt_ >= kAutostartEvery) {
        autostart_ = source_.autostart();
        autostartAt_ = now;
    }
    return autostart_;
}

Message MachineControl::supervisor()
{
    if (source_.state.empty())
        return nullptr;
    const auto describe = [&](const Message& j) -> Message {
        const int pid = j.value("supervisor", 0);
        return {{"running", pid > 0 && processRunning(pid)},
                {"mode", textOrNull(j, "mode")},
                {"reason", textOrNull(j, "reason")},
                {"error", textOrNull(j, "error")},
                {"enginePid", j.value("engine", 0)},
                {"startedAt", j.value("startedAt", Message(nullptr))},
                {"now", std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count()},
                {"starts", j.value("starts", 0)},
                {"crashes", j.value("crashes", Message::array())},
                {"autostart", autostartName(autostart())}};
    };
    try {
        return describe(readStatus(source_.state));
    } catch (const std::exception&) {
        return describe(Message::object());
    }
}

bool MachineControl::handle(const Message& message, const DocumentServer::Sink& reply)
{
    const std::string op = message.value("op", "");
    if (op != "autostart" && op != "settings" && op != "logs")
        return false;
    if (source_.state.empty()) {
        reply(error(op + " needs the supervisor, and this engine runs without one"));
        return true;
    }
    if (op == "autostart")
        setAutostart(message, reply);
    else if (op == "settings")
        settings(message, reply);
    else
        logs(message, reply);
    return true;
}

void MachineControl::setAutostart(const Message& message, const DocumentServer::Sink& reply)
{
    if (!message.value("on", Message()).is_boolean())
        return reply(error("autostart needs \"on\": true or false"));
    if (const std::optional<std::string> refused = refusal_("Autostart"))
        return reply(error(*refused));
    const bool on = message["on"].get<bool>();
    if (const std::optional<std::string> failed = source_.setAutostart(on))
        return reply(error(*failed));
    {
        std::lock_guard lock(mutex_);
        autostartAt_.reset();
    }
    std::printf("autostart: %s\n", on ? "on" : "off");
    reply({{"type", "machine"}, {"op", "autostart"}, {"on", on}});
}

Message MachineControl::settingsReply(const StartSettings& saved, const std::optional<std::string>& error) const
{
    const Message applied = readStatus(source_.state).value("applied", Message::array());
    const auto source = [&](const char* field, const std::string& running) {
        if (applied.is_array() && std::find(applied.begin(), applied.end(), field) != applied.end())
            return "saved";
        return running.empty() ? "default" : "launch";
    };
    Message reply{{"type", "machine"},
                  {"op", "settings"},
                  {"saved", toJson(saved)},
                  {"running", toJson(source_.running)},
                  {"sources",
                   {{"composition", source("composition", source_.running.composition)},
                    {"mediaRoot", source("mediaRoot", source_.running.mediaRoot)},
                    {"audioInput", source("audioInput", source_.running.audioInput)}}}};
    if (error)
        reply["error"] = *error;
    return reply;
}

void MachineControl::settings(const Message& message, const DocumentServer::Sink& reply)
{
    const std::filesystem::path file = source_.state / kStartSettingsFile;
    StartSettings saved;
    std::optional<std::string> unreadable;
    try {
        saved = readStartSettings(file).value_or(StartSettings{});
    } catch (const std::exception& e) {
        unreadable = e.what();
    }
    std::string* const fields[] = {&saved.composition, &saved.mediaRoot, &saved.audioInput};
    const char* const names[] = {"composition", "mediaRoot", "audioInput"};
    bool write = false;
    for (size_t i = 0; i < 3; ++i) {
        if (!message.contains(names[i]))
            continue;
        if (!message[names[i]].is_string())
            return reply(error(std::string(names[i]) + " must be text"));
        *fields[i] = message[names[i]].get<std::string>();
        write = true;
    }
    if (!write)
        return reply(settingsReply(saved, unreadable));
    if (const std::optional<std::string> refused = refusal_("Settings"))
        return reply(error(*refused));
    if (const std::optional<std::string> refused = startSettingsRefusal(saved))
        return reply(error(*refused));
    if (!saved.audioInput.empty() && source_.audioInput && !source_.audioInput(saved.audioInput))
        return reply(error("audioInput: this machine lists no input called " + saved.audioInput));
    try {
        writeStartSettings(file, saved);
    } catch (const std::exception& e) {
        return reply(error(std::string("cannot save the settings: ") + e.what()));
    }
    std::printf("settings: saved; they apply at the next engine start\n");
    reply(settingsReply(saved, std::nullopt));
}

void MachineControl::logs(const Message& message, const DocumentServer::Sink& reply)
{
    const std::string file = message.value("file", Message()).is_string() ? message["file"].get<std::string>() : "";
    if (file != "engine" && file != "supervisor")
        return reply(error("logs needs \"file\": \"engine\" or \"supervisor\""));
    const Message lines = message.value("lines", Message(int64_t(kDefaultLines)));
    if (!lines.is_number_integer() || lines.get<int64_t>() < 1 || lines.get<int64_t>() > int64_t(kMaxLogLines))
        return reply(error("lines must be a whole number from 1 to " + std::to_string(kMaxLogLines)));
    const LogTail tail = source_.logs.empty() ? LogTail{} : tailLog(source_.logs / (file + ".log"), size_t(lines.get<int64_t>()));
    reply({{"type", "machine"}, {"op", "logs"}, {"file", file}, {"text", tail.text}, {"truncated", tail.truncated}});
}

}
