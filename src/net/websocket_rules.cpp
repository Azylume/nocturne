#include "net/websocket_rules.hpp"

#include "net/lan.hpp"
#include "net/pairing.hpp"
#include "net/socket_compat.hpp"
#include "net/websocket.hpp"
#include "protocol/return_stream.hpp"

#include <algorithm>
#include <stdexcept>

namespace azl {

namespace ws {

Message undecodable()
{
    return {{"type", "error"}, {"message", "undecodable message"}};
}

Message unpaired()
{
    return {{"type", "error"},
            {"message", "this engine lets in other machines only with its pairing token in hello (\"token\"): run "
                        "nocturne-ctl token on the engine's machine"}};
}

Message failed(const std::exception& e)
{
    return {{"type", "error"}, {"message", e.what()}};
}

std::string typeOf(const Message& message)
{
    const auto type = message.find("type");
    return type != message.end() && type->is_string() ? type->get<std::string>() : "";
}

bool presentsToken(const std::optional<Message>& message, const std::string& token)
{
    if (!message || typeOf(*message) != "hello")
        return false;
    const auto presented = message->find("token");
    return presented != message->end() && presented->is_string() && sameToken(presented->get<std::string>(), token);
}

bool worthSending(const Message& message, int pending, return_video::Gate& video)
{
    const std::string type = typeOf(message);
    if ((pending > kMaxPendingSends && type == "change") ||
        (pending > kMaxPendingPreviews && type == "preview" && message.contains("jpeg")))
        return false;
    if (!isReturnPacket(message))
        return true;
    const bool congested = pending > kMaxPendingReturn;
    return type == "video" ? video.pass(message.value("display", ""), message.value("key", false), congested)
                           : !congested;
}

std::string webSocketUrl(const std::string& host, uint16_t port)
{
    std::string literal = host;
    if (host.find(':') != std::string::npos) {
        if (const size_t zone = literal.find('%'); zone != std::string::npos)
            literal.replace(zone, 1, "%25");
        literal = "[" + literal + "]";
    }
    return "ws://" + literal + ":" + std::to_string(port) + "/";
}

std::string joined(const std::vector<std::string>& addresses)
{
    std::string text;
    for (const std::string& address : addresses)
        text += (text.empty() ? "" : ", ") + address;
    return text;
}

} // namespace ws

std::vector<std::string> resolveHost(const std::string& host)
{
    startSockets();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (const int error = getaddrinfo(host.c_str(), nullptr, &hints, &found); error != 0)
        throw std::runtime_error("cannot resolve host \"" + host + "\": " + gai_strerror(error));
    std::vector<std::string> addresses;
    for (const addrinfo* a = found; a; a = a->ai_next) {
        char text[NI_MAXHOST];
        if (getnameinfo(a->ai_addr, socklen_t(a->ai_addrlen), text, sizeof text, nullptr, 0, NI_NUMERICHOST) == 0 &&
            std::find(addresses.begin(), addresses.end(), text) == addresses.end())
            addresses.push_back(text);
    }
    freeaddrinfo(found);
    return addresses;
}

bool onThisMachine(const std::string& host)
{
    std::vector<std::string> addresses;
    try {
        addresses = resolveHost(host);
    } catch (const std::exception&) {
        return false;
    }
    const std::vector<std::string> own = interfaceAddresses();
    return !addresses.empty() && std::all_of(addresses.begin(), addresses.end(), [&](const std::string& address) {
        return loopbackAddress(address) || std::find(own.begin(), own.end(), address) != own.end();
    });
}

} // namespace azl
