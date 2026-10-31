#include "sync/udp_clock.hpp"

#include "sync/udp_clock_packet.hpp"

#import <Network/Network.h>

#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>

namespace azl {

namespace {

using namespace udp_clock;

constexpr int64_t kInterval = NSEC_PER_SEC / 10;
constexpr int64_t kSetupTimeout = 5 * int64_t(NSEC_PER_SEC);

nw_parameters_t udpParameters()
{
    return nw_parameters_create_secure_udp(NW_PARAMETERS_DISABLE_PROTOCOL, NW_PARAMETERS_DEFAULT_CONFIGURATION);
}

void sendPacket(nw_connection_t connection, const Packet& packet)
{
    dispatch_data_t data = dispatch_data_create(&packet, sizeof packet, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    nw_connection_send(connection, data, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, true, ^(nw_error_t) {});
}

std::optional<Packet> readPacket(dispatch_data_t content)
{
    if (!content)
        return std::nullopt;
    const void* bytes = nullptr;
    size_t size = 0;
    __attribute__((objc_precise_lifetime)) dispatch_data_t contiguous = dispatch_data_create_map(content, &bytes, &size);
    return udp_clock::readPacket(bytes, size);
}

using OnPacket = std::function<void(const Packet&)>;

void receiveLoop(nw_connection_t connection, OnPacket onPacket)
{
    nw_connection_receive_message(connection, ^(dispatch_data_t content, nw_content_context_t, bool, nw_error_t error) {
        if (error) {
            nw_connection_cancel(connection);
            return;
        }
        if (auto packet = readPacket(content))
            onPacket(*packet);
        receiveLoop(connection, onPacket);
    });
}

} // namespace

// All state is touched on `queue` only.
struct ClockResponder::Impl : std::enable_shared_from_this<Impl> {
    Clock now;
    dispatch_queue_t queue = dispatch_queue_create("nocturne.clock.responder", DISPATCH_QUEUE_SERIAL);
    nw_listener_t listener = nil;
    std::map<uint64_t, nw_connection_t> peers;
    uint64_t nextPeer = 0;
    bool stopped = false;

    explicit Impl(Clock n) : now(std::move(n)) {}

    void accept(nw_connection_t connection)
    {
        if (peers.size() >= kMaxPeers) {
            nw_connection_cancel(peers.begin()->second);
            peers.erase(peers.begin());
        }
        const uint64_t id = nextPeer++;
        peers[id] = connection;
        std::weak_ptr<Impl> weak = weak_from_this();
        nw_connection_set_queue(connection, queue);
        nw_connection_set_state_changed_handler(connection, ^(nw_connection_state_t state, nw_error_t) {
            if (state == nw_connection_state_failed)
                nw_connection_cancel(connection);
            auto self = weak.lock();
            if (state == nw_connection_state_cancelled && self)
                self->peers.erase(id);
        });
        nw_connection_start(connection);
        receiveLoop(connection, [weak, connection](const Packet& ping) {
            auto self = weak.lock();
            if (!self || self->stopped)
                return;
            Packet pong = ping;
            pong.remoteReceived = self->now();
            pong.remoteSent = self->now();
            sendPacket(connection, pong);
        });
    }

    void stop()
    {
        stopped = true;
        for (auto& [id, connection] : peers)
            nw_connection_cancel(connection);
        peers.clear();
        if (listener)
            nw_listener_cancel(listener);
    }
};

ClockResponder::ClockResponder(uint16_t port, Clock now) : impl_(std::make_shared<Impl>(std::move(now)))
{
    Impl* impl = impl_.get();
    std::weak_ptr<Impl> weak = impl_;
    impl->listener = nw_listener_create_with_port(std::to_string(port).c_str(), udpParameters());
    if (!impl->listener)
        throw std::runtime_error("cannot create a clock listener on UDP port " + std::to_string(port));
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
        throw std::runtime_error("cannot listen on UDP port " + std::to_string(port));
    }
    port_ = nw_listener_get_port(impl->listener);
}

ClockResponder::~ClockResponder()
{
    Impl* impl = impl_.get();
    dispatch_sync(impl->queue, ^{ impl->stop(); });
}

struct ClockPinger::Impl {
    Clock now;
    dispatch_queue_t queue = dispatch_queue_create("nocturne.clock.pinger", DISPATCH_QUEUE_SERIAL);
    nw_connection_t connection = nil;
    dispatch_source_t timer = nil;
    uint32_t seq = 0;
    bool stopped = false;
    mutable std::mutex mutex;
    ClockEstimator estimator;

    explicit Impl(Clock n) : now(std::move(n)) {}
};

ClockPinger::ClockPinger(const std::string& host, uint16_t port, Clock now)
    : impl_(std::make_shared<Impl>(std::move(now)))
{
    Impl* impl = impl_.get();
    std::weak_ptr<Impl> weak = impl_;
    nw_endpoint_t endpoint = nw_endpoint_create_host(host.c_str(), std::to_string(port).c_str());
    nw_connection_t connection = nw_connection_create(endpoint, udpParameters());
    impl->connection = connection;
    nw_connection_set_queue(connection, impl->queue);
    nw_connection_set_state_changed_handler(connection, ^(nw_connection_state_t state, nw_error_t) {
        if (state == nw_connection_state_failed)
            nw_connection_cancel(connection);
    });
    nw_connection_start(impl->connection);
    receiveLoop(impl->connection, [weak](const Packet& pong) {
        auto self = weak.lock();
        if (!self || self->stopped || self->seq - pong.seq > kMaxSeqLag)
            return;
        const double received = self->now();
        std::lock_guard lock(self->mutex);
        self->estimator.add({pong.sent, pong.remoteReceived, pong.remoteSent, received});
    });

    impl->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, impl->queue);
    dispatch_source_set_timer(impl->timer, dispatch_time(DISPATCH_TIME_NOW, 0), kInterval, kInterval / 20);
    dispatch_source_set_event_handler(impl->timer, ^{
        auto self = weak.lock();
        if (!self || self->stopped)
            return;
        Packet ping;
        ping.seq = ++self->seq;
        ping.sent = self->now();
        sendPacket(self->connection, ping);
    });
    dispatch_resume(impl->timer);
}

ClockPinger::~ClockPinger()
{
    Impl* impl = impl_.get();
    dispatch_sync(impl->queue, ^{
        impl->stopped = true;
        dispatch_source_cancel(impl->timer);
        nw_connection_cancel(impl->connection);
    });
}

std::optional<double> ClockPinger::offsetAt(double local) const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->estimator.offsetAt(local);
}

double ClockPinger::roundTrip() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->estimator.roundTrip();
}

double ClockPinger::drift() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->estimator.drift();
}

} // namespace azl
