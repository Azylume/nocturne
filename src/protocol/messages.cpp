#include "protocol/messages.hpp"

#include "effects/effect_registry.hpp"
#include "protocol/build_info.hpp"

#include <algorithm>

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

namespace {

constexpr size_t kMaxDepth = 64;

uint64_t bigEndian(const uint8_t* bytes, size_t count)
{
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i)
        value = value << 8 | bytes[i];
    return value;
}

// Walks the MessagePack items without recursion. False when containers nest deeper than kMaxDepth or the bytes end
// before the items they announce.
bool shallowMsgpack(const uint8_t* data, size_t size)
{
    std::vector<uint64_t> open{1};
    size_t at = 0;
    const auto skip = [&](uint64_t count) {
        if (size - at < count)
            return false;
        at += size_t(count);
        return true;
    };
    const auto length = [&](size_t count, uint64_t& value) {
        if (size - at < count)
            return false;
        value = bigEndian(data + at, count);
        at += count;
        return true;
    };
    while (!open.empty()) {
        if (open.back() == 0) {
            open.pop_back();
            continue;
        }
        --open.back();
        if (at == size)
            return false;
        const uint8_t b = data[at++];
        uint64_t n = 0;
        std::optional<uint64_t> items;
        if (b <= 0x7f || b >= 0xe0 || b == 0xc0 || b == 0xc2 || b == 0xc3) {
            continue;
        } else if (b <= 0x8f) {
            items = 2 * uint64_t(b & 0x0f);
        } else if (b <= 0x9f) {
            items = b & 0x0f;
        } else if (b <= 0xbf) {
            if (!skip(b & 0x1f))
                return false;
        } else if (b == 0xc4 || b == 0xc5 || b == 0xc6 || b == 0xd9 || b == 0xda || b == 0xdb) {
            const size_t bytes = b == 0xc4 || b == 0xd9 ? 1 : b == 0xc5 || b == 0xda ? 2 : 4;
            if (!length(bytes, n) || !skip(n))
                return false;
        } else if (b == 0xc7 || b == 0xc8 || b == 0xc9) {
            if (!length(b == 0xc7 ? 1 : b == 0xc8 ? 2 : 4, n) || !skip(n + 1))
                return false;
        } else if (b >= 0xca && b <= 0xd3) {
            static constexpr uint8_t kSizes[] = {4, 8, 1, 2, 4, 8, 1, 2, 4, 8};
            if (!skip(kSizes[b - 0xca]))
                return false;
        } else if (b >= 0xd4 && b <= 0xd8) {
            if (!skip(1 + (uint64_t(1) << (b - 0xd4))))
                return false;
        } else if (b == 0xdc || b == 0xdd || b == 0xde || b == 0xdf) {
            if (!length(b == 0xdc || b == 0xde ? 2 : 4, n))
                return false;
            items = b >= 0xde ? 2 * n : n;
        } else {
            return false;
        }
        if (items) {
            if (open.size() > kMaxDepth)
                return false;
            open.push_back(*items);
        }
    }
    return true;
}

bool utf8(const std::string& text)
{
    try {
        return !Message(text).dump().empty();
    } catch (const Message::type_error&) {
        return false;
    }
}

bool allUtf8(const Message& m)
{
    if (m.is_string())
        return utf8(m.get_ref<const std::string&>());
    if (m.is_object())
        for (auto it = m.begin(); it != m.end(); ++it)
            if (!utf8(it.key()) || !allUtf8(it.value()))
                return false;
    if (m.is_array())
        return std::all_of(m.begin(), m.end(), allUtf8);
    return true;
}

std::optional<Message> decodeMsgpack(const uint8_t* data, size_t size)
{
    if (!shallowMsgpack(data, size))
        return std::nullopt;
    Message m = Message::from_msgpack(data, data + size, true, false);
    if (m.is_discarded() || !allUtf8(m))
        return std::nullopt;
    return m;
}

std::optional<Message> decodeJson(const uint8_t* data, size_t size)
{
    bool tooDeep = false;
    Message m = Message::parse(
        data, data + size,
        [&](int depth, Message::parse_event_t event, Message&) {
            if ((event == Message::parse_event_t::object_start || event == Message::parse_event_t::array_start) &&
                size_t(depth) >= kMaxDepth)
                tooDeep = true;
            return !tooDeep;
        },
        false);
    if (tooDeep || m.is_discarded())
        return std::nullopt;
    return m;
}

} // namespace

std::vector<uint8_t> encode(const Message& message, Encoding encoding)
{
    if (encoding == Encoding::MessagePack)
        return Message::to_msgpack(message);
    const std::string text = message.dump(-1, ' ', false, Message::error_handler_t::replace);
    return {text.begin(), text.end()};
}

std::optional<Message> decode(const uint8_t* data, size_t size, Encoding encoding)
{
    std::optional<Message> m = encoding == Encoding::MessagePack ? decodeMsgpack(data, size) : decodeJson(data, size);
    if (!m || !m->is_object())
        return std::nullopt;
    return m;
}

} // namespace azl
