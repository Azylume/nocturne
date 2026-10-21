#include "engine/isf_service.hpp"

namespace azl {

namespace {

const char* kindName(IsfKind kind)
{
    switch (kind) {
    case IsfKind::Effect: return "effect";
    case IsfKind::Generator: return "generator";
    case IsfKind::Transition: return "transition";
    }
    return "effect";
}

} // namespace

IsfService::IsfService(DocumentServer& document, UserIsfLibrary& library, std::function<void()> changed)
    : document_(document), library_(library), changed_(std::move(changed))
{
    document_.handle("isf", [this](const Message& message, DocumentServer::Sink reply) { handle(message, reply); });
}

IsfService::~IsfService()
{
    document_.handle("isf", nullptr);
}

void IsfService::handle(const Message& message, const DocumentServer::Sink& reply)
{
    const std::string op = message.value("op", "");
    const bool text = message.value("text", false);
    if (op == "list")
        return reply(list("list", text));
    if (op != "rescan")
        return reply({{"type", "error"}, {"message", "unknown isf op " + op}});
    const bool changed = library_.scan();
    if (changed && changed_)
        changed_();
    reply(list("rescan", text));
    if (changed)
        document_.broadcast(list("list", true));
}

Message IsfService::list(const char* op, bool text) const
{
    Message shaders = Message::array();
    for (const UserIsfShader& s : library_.shaders()) {
        Message entry{{"file", s.file}, {"name", s.name}, {"type", s.type}, {"kind", kindName(s.kind)},
                      {"error", s.error}};
        if (text)
            entry["text"] = s.text;
        shaders.push_back(std::move(entry));
    }
    return {{"type", "isf"}, {"op", op}, {"folder", library_.folder().string()}, {"shaders", std::move(shaders)}};
}

} // namespace azl
