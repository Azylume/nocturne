#include "engine/machine_actions.hpp"

#include "document/composition_file.hpp"

#include <cstdio>

namespace azl {

namespace {

Message error(const std::string& text)
{
    return {{"type", "error"}, {"message", text}};
}

bool readLock(const std::filesystem::path& file)
{
    if (file.empty())
        return false;
    try {
        const std::optional<nlohmann::json> j = readJsonFile(file);
        return j && (!j->is_object() || j->value("on", true));
    } catch (const std::exception& e) {
        std::printf("show lock: %s; keeping it on\n", e.what());
        return true;
    }
}

} // namespace

MachineActions::MachineActions(std::filesystem::path lockFile, Restart restart, Stop stop)
    : lockFile_(std::move(lockFile)), restart_(std::move(restart)), stop_(std::move(stop)), locked_(readLock(lockFile_))
{
    if (locked_)
        std::printf("show lock: on\n");
}

bool MachineActions::handle(const Message& message, const DocumentServer::Sink& reply)
{
    const std::string op = message.value("op", "");
    const ClientId client = message.value("client", ClientId(0));
    if (op == "restart")
        restart(client, false, "restart", "Restart engine", reply);
    else if (op == "safeMode")
        restart(client, true, "safeMode", "Safe mode", reply);
    else if (op == "stop")
        stop(client, reply);
    else if (op == "lock")
        lock(message, reply);
    else
        return false;
    return true;
}

bool MachineActions::locked() const
{
    std::lock_guard lock(mutex_);
    return locked_;
}

std::optional<std::string> MachineActions::refusal(const std::string& action) const
{
    if (!locked())
        return std::nullopt;
    return "Show lock is on: " + action + " is refused until it is turned off";
}

void MachineActions::restart(ClientId client, bool safeMode, const char* op, const char* action,
                             const DocumentServer::Sink& reply)
{
    if (const std::optional<std::string> refused = refusal(action))
        return reply(error(*refused));
    if (!restart_)
        return reply(error(std::string(action) + " needs the supervisor, and this engine runs without one"));
    std::printf("%s asked for from client %llu\n", action, (unsigned long long)client);
    std::fflush(stdout);
    reply({{"type", "machine"}, {"op", op}});
    restart_(safeMode);
}

void MachineActions::stop(ClientId client, const DocumentServer::Sink& reply)
{
    if (const std::optional<std::string> refused = refusal("Stop"))
        return reply(error(*refused));
    if (!stop_)
        return reply(error("Stop needs the supervisor, and this engine runs without one"));
    std::printf("Stop asked for from client %llu\n", (unsigned long long)client);
    std::fflush(stdout);
    reply({{"type", "machine"}, {"op", "stop"}});
    stop_();
}

void MachineActions::lock(const Message& message, const DocumentServer::Sink& reply)
{
    if (!message.value("on", Message()).is_boolean())
        return reply(error("lock needs \"on\": true or false"));
    const bool on = message["on"].get<bool>();
    {
        std::lock_guard lock(mutex_);
        if (!lockFile_.empty()) {
            try {
                writeFileAtomically(lockFile_, nlohmann::json{{"on", on}}.dump() + "\n");
            } catch (const std::exception& e) {
                return reply(error(std::string("cannot save the Show lock: ") + e.what()));
            }
        }
        locked_ = on;
    }
    std::printf("show lock: %s\n", on ? "on" : "off");
    reply({{"type", "machine"}, {"op", "lock"}, {"on", on}});
}

} // namespace azl
