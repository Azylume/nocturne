#include "net/websocket.hpp"

#include "net/lan.hpp"
#include "net/pairing.hpp"
#include "output/return_video.hpp"
#include "protocol/return_stream.hpp"

#import <Network/Network.h>

#include <ifaddrs.h>
#include <netdb.h>
#include <strings.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <optional>
#include <stdexcept>

namespace azl {

namespace {

// A client this far behind stops receiving changes; it notices the gap and re-requests a snapshot.
constexpr int kMaxPendingSends = 256;
// Preview pictures are dropped sooner: the next one replaces them anyway.
constexpr int kMaxPendingPreviews = 4;
constexpr int64_t kSetupTimeout = 5 * int64_t(NSEC_PER_SEC);
// Room for the largest message a client sends, a 1 MiB bundle chunk, even as JSON.
constexpr size_t kMaxMessageBytes = size_t(16) << 20;

bool fromLoopback(nw_connection_t connection)
{
    nw_endpoint_t endpoint = nw_connection_copy_endpoint(connection);
    if (!endpoint || nw_endpoint_get_type(endpoint) != nw_endpoint_type_address)
        return false;
    char* text = nw_endpoint_copy_address_string(endpoint);
    const bool loopback = text && loopbackAddress(text);
    free(text);
    return loopback;
}

nw_parameters_t webSocketParameters(nw_protocol_options_t ws = nw_ws_create_options(nw_ws_version_13))
{
    nw_parameters_t params =
        nw_parameters_create_secure_tcp(NW_PARAMETERS_DISABLE_PROTOCOL, NW_PARAMETERS_DEFAULT_CONFIGURATION);
    nw_ws_options_set_auto_reply_ping(ws, true);
    nw_protocol_stack_t stack = nw_parameters_copy_default_protocol_stack(params);
    nw_protocol_stack_prepend_application_protocol(stack, ws);
    return params;
}

// Browsers send Origin on every WebSocket handshake and native clients never do: refusing it keeps web pages the
// user opens from reaching the engine through loopback.
nw_parameters_t serverParameters(dispatch_queue_t queue)
{
    nw_protocol_options_t ws = nw_ws_create_options(nw_ws_version_13);
    nw_ws_options_set_maximum_message_size(ws, kMaxMessageBytes);
    nw_ws_options_set_client_request_handler(ws, queue, ^nw_ws_response_t(nw_ws_request_t request) {
        __block bool origin = false;
        nw_ws_request_enumerate_additional_headers(request, ^bool(const char* name, const char*) {
            origin = strcasecmp(name, "Origin") == 0;
            return !origin;
        });
        return nw_ws_response_create(origin ? nw_ws_response_status_reject : nw_ws_response_status_accept, nullptr);
    });
    return webSocketParameters(ws);
}

// One open WebSocket, on either side.
struct Peer {
    nw_connection_t connection;
    std::atomic<Encoding> encoding;
    std::atomic<int> pending{0};
    return_video::Gate video;
    // Server side, touched on its queue only: its messages reach the document (loopback, or a hello with the token
    // came first); refused, it is being closed.
    bool paired = true;
    bool refused = false;

    Peer(nw_connection_t c, Encoding e) : connection(c), encoding(e) {}
};

// By value: the completion block copies it, while a reference would dangle. `close` closes the connection once the
// message is out.
void send(std::shared_ptr<Peer> peer, const Message& message, bool close = false)
{
    const Encoding encoding = peer->encoding;
    const std::vector<uint8_t> bytes = encode(message, encoding);
    dispatch_data_t data = dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    nw_protocol_metadata_t metadata =
        nw_ws_create_metadata(encoding == Encoding::Json ? nw_ws_opcode_text : nw_ws_opcode_binary);
    nw_content_context_t context = nw_content_context_create("message");
    nw_content_context_set_metadata_for_protocol(context, metadata);
    peer->pending++;
    nw_connection_send(peer->connection, data, context, true, ^(nw_error_t) {
        peer->pending--;
        if (close)
            nw_connection_cancel(peer->connection);
    });
}

std::optional<nw_ws_opcode_t> opcode(nw_content_context_t context)
{
    if (!context)
        return std::nullopt;
    nw_protocol_metadata_t metadata =
        nw_content_context_copy_protocol_metadata(context, nw_protocol_copy_ws_definition());
    if (!metadata)
        return std::nullopt;
    return nw_ws_metadata_get_opcode(metadata);
}

std::optional<Message> decodeData(dispatch_data_t content, Encoding encoding)
{
    const void* bytes = nullptr;
    size_t size = 0;
    __attribute__((objc_precise_lifetime)) dispatch_data_t contiguous =
        dispatch_data_create_map(content, &bytes, &size);
    return decode(static_cast<const uint8_t*>(bytes), size, encoding);
}

using OnReceived = std::function<void(const std::optional<Message>&)>;

// Hands every received message to `onReceived` (null when undecodable) until the connection closes, then cancels it.
void receiveLoop(std::shared_ptr<Peer> peer, OnReceived onReceived)
{
    nw_connection_receive_message(
        peer->connection, ^(dispatch_data_t content, nw_content_context_t context, bool, nw_error_t error) {
            const std::optional<nw_ws_opcode_t> op = opcode(context);
            if (error || op == nw_ws_opcode_close) {
                nw_connection_cancel(peer->connection);
                return;
            }
            if (content) {
                peer->encoding = op == nw_ws_opcode_text ? Encoding::Json : Encoding::MessagePack;
                onReceived(decodeData(content, peer->encoding));
            }
            receiveLoop(peer, onReceived);
        });
}

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

// Empty when the message has no "type" or it is not a string.
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

std::vector<std::string> interfaceAddresses()
{
    std::vector<std::string> addresses;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0)
        return addresses;
    for (const ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || (a->ifa_addr->sa_family != AF_INET && a->ifa_addr->sa_family != AF_INET6))
            continue;
        const socklen_t size = a->ifa_addr->sa_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
        char text[NI_MAXHOST];
        if (getnameinfo(a->ifa_addr, size, text, sizeof text, nullptr, 0, NI_NUMERICHOST) == 0)
            addresses.push_back(text);
    }
    freeifaddrs(list);
    return addresses;
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

} // namespace

std::vector<std::string> resolveHost(const std::string& host)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (const int error = getaddrinfo(host.c_str(), nullptr, &hints, &found); error != 0)
        throw std::runtime_error("cannot resolve host \"" + host + "\": " + gai_strerror(error));
    std::vector<std::string> addresses;
    for (const addrinfo* a = found; a; a = a->ai_next) {
        char text[NI_MAXHOST];
        if (getnameinfo(a->ai_addr, a->ai_addrlen, text, sizeof text, nullptr, 0, NI_NUMERICHOST) == 0 &&
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

// All state is touched on `queue` only. Network callbacks hold it weakly and do nothing once stopped.
struct WebSocketServer::Impl : std::enable_shared_from_this<Impl> {
    DocumentServer& document;
    const Pairing pairing;
    dispatch_queue_t queue = dispatch_queue_create("nocturne.websocket.server", DISPATCH_QUEUE_SERIAL);
    nw_listener_t listener = nil;
    std::map<ClientId, std::shared_ptr<Peer>> peers;
    bool stopped = false;

    Impl(DocumentServer& d, Pairing p) : document(d), pairing(std::move(p)) {}

    void accept(nw_connection_t connection)
    {
        auto peer = std::make_shared<Peer>(connection, Encoding::MessagePack);
        const bool loopback = pairing.trustLoopback && fromLoopback(connection);
        peer->paired = loopback;
        const ClientId id = document.connect([weakPeer = std::weak_ptr<Peer>(peer)](const Message& message) {
            auto p = weakPeer.lock();
            const std::string type = typeOf(message);
            if (!p || (p->pending > kMaxPendingSends && type == "change") ||
                (p->pending > kMaxPendingPreviews && type == "preview" && message.contains("jpeg")))
                return;
            if (isReturnPacket(message)) {
                const bool congested = p->pending > kMaxPendingReturn;
                if (type == "video" ? !p->video.pass(message.value("display", ""), message.value("key", false), congested)
                                    : congested)
                    return;
            }
            send(p, message);
        }, loopback);
        peers[id] = peer;

        std::weak_ptr<Impl> weak = weak_from_this();
        nw_connection_set_queue(connection, queue);
        nw_connection_set_state_changed_handler(connection, ^(nw_connection_state_t state, nw_error_t) {
            if (state == nw_connection_state_failed)
                nw_connection_cancel(connection);
            auto self = weak.lock();
            if (state == nw_connection_state_cancelled && self && !self->stopped)
                self->drop(id);
        });
        nw_connection_start(connection);
        receiveLoop(peer, [weak, id, peer = std::weak_ptr<Peer>(peer)](const std::optional<Message>& message) {
            auto self = weak.lock();
            auto p = peer.lock();
            if (!self || self->stopped || !p || p->refused)
                return;
            if (!p->paired && !presentsToken(message, self->pairing.token)) {
                p->refused = true;
                send(p, unpaired(), true);
                return;
            }
            p->paired = true;
            if (message)
                self->document.receive(id, *message);
            else
                send(p, undecodable());
        });
    }

    void drop(ClientId id)
    {
        peers.erase(id);
        document.disconnect(id);
    }

    void stop()
    {
        stopped = true;
        for (auto& [id, peer] : peers) {
            document.disconnect(id);
            nw_connection_cancel(peer->connection);
        }
        peers.clear();
        if (listener)
            nw_listener_cancel(listener);
    }
};

WebSocketServer::WebSocketServer(DocumentServer& document, uint16_t port, Pairing pairing)
    : impl_(std::make_shared<Impl>(document, std::move(pairing)))
{
    Impl* impl = impl_.get();
    std::weak_ptr<Impl> weak = impl_;
    impl->listener = nw_listener_create_with_port(std::to_string(port).c_str(), serverParameters(impl->queue));
    if (!impl->listener)
        throw std::runtime_error("cannot create a listener on port " + std::to_string(port));

    dispatch_semaphore_t settled = dispatch_semaphore_create(0);
    __block bool ready = false;
    nw_listener_set_queue(impl->listener, impl->queue);
    nw_listener_set_state_changed_handler(impl->listener, ^(nw_listener_state_t state, nw_error_t) {
        if (state == nw_listener_state_ready)
            ready = true;
        if (state != nw_listener_state_waiting && state != nw_listener_state_invalid)
            dispatch_semaphore_signal(settled);
    });
    nw_listener_set_new_connection_handler(impl->listener, ^(nw_connection_t connection) {
        auto self = weak.lock();
        if (!self || self->stopped) {
            nw_connection_cancel(connection);
            return;
        }
        self->accept(connection);
    });
    nw_listener_start(impl->listener);

    const bool settledInTime = dispatch_semaphore_wait(settled, dispatch_time(DISPATCH_TIME_NOW, kSetupTimeout)) == 0;
    __block bool listening = false;
    dispatch_sync(impl->queue, ^{ listening = settledInTime && ready; });
    if (!listening) {
        dispatch_sync(impl->queue, ^{ impl->stop(); });
        throw std::runtime_error("cannot listen on port " + std::to_string(port));
    }
    port_ = nw_listener_get_port(impl->listener);
}

WebSocketServer::~WebSocketServer()
{
    Impl* impl = impl_.get();
    dispatch_sync(impl->queue, ^{ impl->stop(); });
}

struct WebSocketClient::Impl {
    dispatch_queue_t queue = dispatch_queue_create("nocturne.websocket.client", DISPATCH_QUEUE_SERIAL);
    std::shared_ptr<Peer> peer;
    bool stopped = false;
};

WebSocketClient::WebSocketClient(const std::string& host, uint16_t port, Encoding encoding, OnMessage onMessage,
                                 OnClose onClose)
    : impl_(std::make_shared<Impl>())
{
    const std::vector<std::string> addresses = resolveHost(host);
    Impl* impl = impl_.get();
    std::weak_ptr<Impl> weak = impl_;
    const std::string url = webSocketUrl(host, port);
    nw_endpoint_t endpoint = nw_endpoint_create_url(url.c_str());
    nw_connection_t connection = nw_connection_create(endpoint, webSocketParameters());
    impl->peer = std::make_shared<Peer>(connection, encoding);

    dispatch_semaphore_t settled = dispatch_semaphore_create(0);
    __block bool open = false;
    __block bool setup = true;
    nw_connection_set_queue(connection, impl->queue);
    nw_connection_set_state_changed_handler(connection, ^(nw_connection_state_t state, nw_error_t) {
        if (state == nw_connection_state_ready && setup)
            open = true;
        const bool ended = state == nw_connection_state_waiting || state == nw_connection_state_failed;
        if (ended)
            nw_connection_cancel(connection);
        if (setup && (open || ended)) {
            setup = false;
            dispatch_semaphore_signal(settled);
        }
        auto self = weak.lock();
        if (state == nw_connection_state_cancelled && self && !self->stopped && onClose)
            onClose();
    });
    nw_connection_start(connection);
    receiveLoop(impl->peer, [weak, onMessage](const std::optional<Message>& message) {
        auto self = weak.lock();
        if (self && !self->stopped && message)
            onMessage(*message);
    });

    const bool settledInTime = dispatch_semaphore_wait(settled, dispatch_time(DISPATCH_TIME_NOW, kSetupTimeout)) == 0;
    __block bool connected = false;
    dispatch_sync(impl->queue, ^{
        setup = false;
        connected = settledInTime && open;
    });
    if (!connected) {
        dispatch_sync(impl->queue, ^{
            impl->stopped = true;
            nw_connection_cancel(connection);
        });
        const bool named = addresses != std::vector<std::string>{host};
        throw std::runtime_error("cannot connect to " + url + (named ? " (" + joined(addresses) + ")" : ""));
    }
}

WebSocketClient::~WebSocketClient()
{
    Impl* impl = impl_.get();
    dispatch_sync(impl->queue, ^{
        impl->stopped = true;
        nw_connection_cancel(impl->peer->connection);
    });
}

void WebSocketClient::send(const Message& message)
{
    azl::send(impl_->peer, message);
}

} // namespace azl
