#pragma once

#include "app/engine_config.hpp"
#include "engine/machine_actions.hpp"
#include "net/websocket.hpp"
#include "protocol/build_info.hpp"

#include <memory>
#include <string>

namespace azl {

// The machine status and the guarded actions, and nothing of the document.
class SafeModeProtocol {
public:
    SafeModeProtocol(const app::EngineConfig& opt, MachineActions::Restart restart, MachineActions::Stop stop)
        : reason_(opt.safeMode), document_(Composition{}, [](auto) {}),
          actions_(opt.state.empty() ? std::filesystem::path() : std::filesystem::path(opt.state) / "show-lock.json",
                   std::move(restart), std::move(stop))
    {
        document_.setReadOnly("the engine is in safe mode: " + reason_, {"machine"});
        document_.handle("machine", [this](const Message& message, DocumentServer::Sink reply) {
            handle(message, reply);
        });
        try {
            server_ = std::make_unique<WebSocketServer>(document_, opt.port);
            std::printf("protocol on ws://0.0.0.0:%u (safe mode)\n", unsigned(server_->port()));
        } catch (const std::exception& e) {
            std::printf("protocol: %s\n", e.what());
        }
    }
    ~SafeModeProtocol()
    {
        server_.reset();
        document_.handle("machine", nullptr);
    }

private:
    void handle(const Message& message, const DocumentServer::Sink& reply)
    {
        const std::string op = message.value("op", "");
        if (op != "safeMode" && actions_.handle(message, reply))
            return;
        if (op == "unsubscribe")
            return;
        if (op != "status" && op != "subscribe")
            return reply({{"type", "error"}, {"message", "the engine is in safe mode: " + op + " is not served"}});
        reply({{"type", "machine"},
               {"op", "status"},
               {"mode", "safe"},
               {"reason", reason_},
               {"showLock", actions_.locked()},
               {"version", buildId()},
               {"outputs", Message::array()}});
    }

    std::string reason_;
    DocumentServer document_;
    MachineActions actions_;
    std::unique_ptr<WebSocketServer> server_;
};

} // namespace azl
