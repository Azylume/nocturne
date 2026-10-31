#include "net/websocket.hpp"

#include "net/lan.hpp"
#include "net/websocket_rules.hpp"
#include "output/return_video.hpp"

#import <Network/Network.h>

#include <strings.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <optional>
#include <stdexcept>

namespace azl {

namespace {

using namespace ws;

constexpr int64_t kSetupTimeout = 5 * int64_t(NSEC_PER_SEC);

std::string remoteAddress(nw_connection_t connection)
{
    nw_endpoint_t endpoint = nw_connection_copy_endpoint(connection);
    if (!endpoint || nw_endpoint_get_type(endpoint) != nw_endpoint_type_address)
        return {};
    char* text = nw_endpoint_copy_address_string(endpoint);
    std::string address = text ? text : "";
    free(text);
    return address;
}

bool fromLoopback(nw_connection_t connection)
{
    const std::string address = remoteAddress(connection);
    return !address.empty() && loopbackAddress(address);
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
    std::string address;

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

using OnReceived = std::function<void(dispatch_data_t)>;

// Hands every received message's bytes to `onReceived` until the connection closes, then cancels it.
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
                onReceived(content);
            }
            receiveLoop(peer, onReceived);
        });
}

} // namespace

// All state is touched on `queue` only. Network callbacks hold it weakly and do nothing once stopped.
struct WebSocketServer::Impl : std::enable_shared_from_this<Impl> {
    DocumentServer& document;
    const Pairing pairing;
    dispatch_queue_t queue = dispatch_queue_create("nocturne.websocket.server", DISPATCH_QUEUE_SERIAL);
    nw_listener_t listener = nil;
    std::map<ClientId, std::shared_ptr<Peer>> peers;
    bool stopped = false;

    Impl(DocumentServer& d, Pairing p) : document(d), pairing(std::move(p)) {}

    size_t unpairedCount() const
    {
        return std::count_if(peers.begin(), peers.end(), [](const auto& entry) { return !entry.second->paired; });
    }

    size_t unpairedFrom(const std::string& address) const
    {
        return std::count_if(peers.begin(), peers.end(), [&](const auto& entry) {
            return !entry.second->paired && entry.second->address == address;
        });
    }

    void accept(nw_connection_t connection)
    {
        const bool loopback = pairing.trustLoopback && fromLoopback(connection);
        const std::string address = remoteAddress(connection);
        if (!loopback && (unpairedCount() >= pairing.maxUnpaired ||
                          unpairedFrom(address) >= pairing.maxUnpairedPerHost)) {
            nw_connection_cancel(connection);
            return;
        }
        auto peer = std::make_shared<Peer>(connection, Encoding::MessagePack);
        peer->paired = loopback;
        peer->address = address;
        const ClientId id = document.connect([weakPeer = std::weak_ptr<Peer>(peer)](const Message& message) {
            auto p = weakPeer.lock();
            if (p && worthSending(message, p->pending, p->video))
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
        std::weak_ptr<Peer> weakPeer = peer;
        if (!loopback)
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, std::chrono::nanoseconds(pairing.helloTimeout).count()),
                           queue, ^{
                               auto self = weak.lock();
                               auto p = weakPeer.lock();
                               if (self && !self->stopped && p && !p->paired)
                                   nw_connection_cancel(connection);
                           });
        receiveLoop(peer, [weak, id, peer = std::weak_ptr<Peer>(peer)](dispatch_data_t content) {
            auto self = weak.lock();
            auto p = peer.lock();
            if (!self || self->stopped || !p || p->refused)
                return;
            try {
                self->receive(id, p, content);
            } catch (const std::exception& e) {
                p->refused = !p->paired;
                send(p, failed(e), p->refused);
            }
        });
    }

    void receive(ClientId id, const std::shared_ptr<Peer>& peer, dispatch_data_t content)
    {
        if (!peer->paired && dispatch_data_get_size(content) > kMaxUnpairedMessageBytes) {
            peer->refused = true;
            send(peer, unpaired(), true);
            return;
        }
        const std::optional<Message> message = decodeData(content, peer->encoding);
        if (!peer->paired && !presentsToken(message, pairing.token)) {
            peer->refused = true;
            send(peer, unpaired(), true);
            return;
        }
        peer->paired = true;
        if (message)
            document.receive(id, *message);
        else
            send(peer, undecodable());
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

WebSocketServer::WebSocketServer(DocumentServer& document, uint16_t port, Pairing pairing, bool loopbackOnly)
    : impl_(std::make_shared<Impl>(document, std::move(pairing)))
{
    Impl* impl = impl_.get();
    std::weak_ptr<Impl> weak = impl_;
    const std::string portText = std::to_string(port);
    if (loopbackOnly) {
        // A local endpoint fixed to 127.0.0.1 binds the loopback interface alone; nothing on the LAN can reach it.
        nw_parameters_t params = serverParameters(impl->queue);
        nw_endpoint_t local = nw_endpoint_create_host("127.0.0.1", portText.c_str());
        nw_parameters_set_local_endpoint(params, local);
        impl->listener = nw_listener_create(params);
    } else {
        impl->listener = nw_listener_create_with_port(portText.c_str(), serverParameters(impl->queue));
    }
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
    receiveLoop(impl->peer, [weak, onMessage, peer = std::weak_ptr<Peer>(impl->peer)](dispatch_data_t content) {
        auto self = weak.lock();
        auto p = peer.lock();
        if (!self || self->stopped || !p)
            return;
        if (const std::optional<Message> message = decodeData(content, p->encoding))
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
