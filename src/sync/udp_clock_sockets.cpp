// The UDP clock (ADR 0005) on plain sockets (Winsock on Windows, POSIX elsewhere), for platforms without
// Network.framework. Same packets and timing as udp_clock.mm.

#include "sync/udp_clock.hpp"

#include "net/socket_compat.hpp"
#include "sync/udp_clock_packet.hpp"

#ifdef _WIN32
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace azl {

namespace {

using namespace udp_clock;
using Steady = std::chrono::steady_clock;

constexpr auto kInterval = std::chrono::milliseconds(100);

// A ping whose sender has gone does not fail the next receive: Windows would report its port unreachable there.
void ignorePortUnreachable(Socket socket)
{
#ifdef _WIN32
    BOOL off = FALSE;
    DWORD bytes = 0;
    WSAIoctl(socket, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &bytes, nullptr, nullptr);
#endif
}

// A UDP socket bound to `port` on every interface, IPv6 and IPv4 where the system has both; none when it cannot bind.
Socket boundUdpSocket(uint16_t port)
{
    for (const int family : {AF_INET6, AF_INET}) {
        const Socket socket = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == kNoSocket)
            continue;
        sockaddr_storage at{};
        socklen_t size = 0;
        if (family == AF_INET6) {
            const int off = 0;
            ::setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof off);
            auto& six = reinterpret_cast<sockaddr_in6&>(at);
            six.sin6_family = AF_INET6;
            six.sin6_port = htons(port);
            six.sin6_addr = in6addr_any;
            size = sizeof six;
        } else {
            in_addr any{};
            any.s_addr = htonl(INADDR_ANY);
            reinterpret_cast<sockaddr_in&>(at) = ipv4Endpoint(any, port);
            size = sizeof(sockaddr_in);
        }
        if (::bind(socket, reinterpret_cast<sockaddr*>(&at), size) == 0 && setNonBlocking(socket)) {
            noInherit(socket);
            ignorePortUnreachable(socket);
            return socket;
        }
        closeSocket(socket);
    }
    return kNoSocket;
}

// A UDP socket connected to the first address `host` resolves to; none while it does not resolve.
Socket connectedUdpSocket(const std::string& host, uint16_t port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0)
        return kNoSocket;
    Socket socket = ::socket(found->ai_family, SOCK_DGRAM, IPPROTO_UDP);
    if (socket != kNoSocket && (::connect(socket, found->ai_addr, socklen_t(found->ai_addrlen)) != 0 ||
                                !setNonBlocking(socket))) {
        closeSocket(socket);
        socket = kNoSocket;
    }
    freeaddrinfo(found);
    if (socket != kNoSocket)
        noInherit(socket);
    return socket;
}

// Waits for the socket or the wake, until `until`.
void waitForPacket(Socket socket, Wake& wake, Steady::time_point until)
{
    pollfd fds[2] = {{wake.socket(), POLLIN, 0}, {socket, POLLIN, 0}};
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Steady::now()).count();
    pollSockets(fds, socket == kNoSocket ? 1 : 2, int(std::max<int64_t>(left, 0)));
    if (fds[0].revents & POLLIN)
        wake.drain();
}

} // namespace

// The socket is read and answered on `thread` only.
struct ClockResponder::Impl {
    Clock now;
    Socket socket = kNoSocket;
    Wake wake;
    std::atomic<bool> stopped{false};
    std::thread thread;

    explicit Impl(Clock n) : now(std::move(n)) {}

    void run()
    {
        while (!stopped) {
            waitForPacket(socket, wake, Steady::now() + std::chrono::seconds(1));
            answerAll();
        }
    }

    void answerAll()
    {
        for (;;) {
            sockaddr_storage from{};
            socklen_t size = sizeof from;
            Packet ping;
            const long n = ::recvfrom(socket, reinterpret_cast<char*>(&ping), sizeof ping, 0,
                                      reinterpret_cast<sockaddr*>(&from), &size);
            if (n < 0 || stopped)
                return;
            std::optional<Packet> pong = readPacket(&ping, size_t(n));
            if (!pong)
                continue;
            pong->remoteReceived = now();
            pong->remoteSent = now();
            ::sendto(socket, reinterpret_cast<const char*>(&*pong), sizeof *pong, 0,
                     reinterpret_cast<const sockaddr*>(&from), size);
        }
    }
};

ClockResponder::ClockResponder(uint16_t port, Clock now) : impl_(std::make_shared<Impl>(std::move(now)))
{
    startSockets();
    impl_->socket = boundUdpSocket(port);
    if (impl_->socket == kNoSocket)
        throw std::runtime_error("cannot listen on UDP port " + std::to_string(port));
    sockaddr_storage at{};
    socklen_t size = sizeof at;
    ::getsockname(impl_->socket, reinterpret_cast<sockaddr*>(&at), &size);
    port_ = ntohs(at.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6&>(at).sin6_port
                                           : reinterpret_cast<sockaddr_in&>(at).sin_port);
    impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
}

ClockResponder::~ClockResponder()
{
    impl_->stopped = true;
    impl_->wake.signal();
    impl_->thread.join();
    closeSocket(impl_->socket);
}

// The socket and `seq` are touched on `thread` only; the estimator under `mutex`.
struct ClockPinger::Impl {
    Clock now;
    std::string host;
    uint16_t port = 0;
    Socket socket = kNoSocket;
    Wake wake;
    uint32_t seq = 0;
    std::atomic<bool> stopped{false};
    mutable std::mutex mutex;
    ClockEstimator estimator;
    std::thread thread;

    Impl(Clock n, std::string h, uint16_t p) : now(std::move(n)), host(std::move(h)), port(p) {}

    // Pings ten times a second, and takes in every pong between pings. A host that does not resolve yet is tried
    // again at each ping.
    void run()
    {
        auto next = Steady::now();
        while (!stopped) {
            if (Steady::now() >= next) {
                ping();
                next += kInterval;
            }
            waitForPacket(socket, wake, next);
            receiveAll();
        }
    }

    void ping()
    {
        if (socket == kNoSocket)
            socket = connectedUdpSocket(host, port);
        if (socket == kNoSocket)
            return;
        Packet packet;
        packet.seq = ++seq;
        packet.sent = now();
        sendSome(socket, &packet, sizeof packet);
    }

    void receiveAll()
    {
        Packet bytes;
        long n = 0;
        while (socket != kNoSocket && !stopped && (n = receiveSome(socket, &bytes, sizeof bytes)) >= 0) {
            const std::optional<Packet> pong = readPacket(&bytes, size_t(n));
            if (!pong || seq - pong->seq > kMaxSeqLag)
                continue;
            const double received = now();
            std::lock_guard lock(mutex);
            estimator.add({pong->sent, pong->remoteReceived, pong->remoteSent, received});
        }
    }
};

ClockPinger::ClockPinger(const std::string& host, uint16_t port, Clock now)
    : impl_(std::make_shared<Impl>(std::move(now), host, port))
{
    startSockets();
    impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
}

ClockPinger::~ClockPinger()
{
    impl_->stopped = true;
    impl_->wake.signal();
    impl_->thread.join();
    if (impl_->socket != kNoSocket)
        closeSocket(impl_->socket);
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
