#include "protocol/messages.hpp"

#include "effects/effect_registry.hpp"
#include "protocol/build_info.hpp"

namespace azl {

namespace msg {

Message hello(const std::string& token)
{
    Message hello{{"type", "hello"}, {"protocol", 0}, {"build", buildId()}, {"effects", effectRegistryVersion()}};
    if (!token.empty())
        hello["token"] = token;
    return hello;
}

Message snapshotRequest()
{
    return {{"type", "snapshot"}};
}

Message command(const Edit& edit)
{
    Message m = editToJson(edit);
    m["type"] = "command";
    return m;
}

Message undo()
{
    return {{"type", "command"}, {"op", "undo"}};
}

Message redo()
{
    return {{"type", "command"}, {"op", "redo"}};
}

Message chainFailed(const std::vector<std::string>& chain, const std::string& error)
{
    return {{"type", "error"}, {"message", "effect chain failed to compile: " + error}, {"chain", chain}};
}

Message clipFailed(uint64_t layer, const std::string& hash, const std::string& reason)
{
    return {{"type", "error"},
            {"message", "clip " + hash + " on layer " + std::to_string(layer) + " cannot play: " + reason},
            {"layer", layer},
            {"hash", hash}};
}

} // namespace msg

std::vector<uint8_t> encode(const Message& message, Encoding encoding)
{
    if (encoding == Encoding::MessagePack)
        return Message::to_msgpack(message);
    const std::string text = message.dump();
    return {text.begin(), text.end()};
}

std::optional<Message> decode(const uint8_t* data, size_t size, Encoding encoding)
{
    Message m = encoding == Encoding::MessagePack ? Message::from_msgpack(data, data + size, true, false)
                                                  : Message::parse(data, data + size, nullptr, false);
    if (m.is_discarded() || !m.is_object())
        return std::nullopt;
    return m;
}

} // namespace azl
