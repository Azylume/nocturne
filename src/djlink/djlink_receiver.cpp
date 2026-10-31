#include "djlink/djlink_receiver.hpp"

#include "djlink/djlink_packets.hpp"
#include "net/socket_compat.hpp"

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <net/if.h>
#endif

#include <chrono>

namespace azl::djlink {

namespace {

constexpr int kPollMs = 250;

// Other Pro DJ Link software on this machine may hold the same ports. On macOS the socket hears `index` alone;
// elsewhere it hears every interface, as no portable call narrows what a socket receives.
bool shareAndBind(Socket fd, unsigned index)
{
    const int on = 1;
    const char* flag = reinterpret_cast<const char*>(&on);
#ifdef __APPLE__
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, flag, sizeof on) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, flag, sizeof on) == 0 &&
           setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &index, sizeof index) == 0;
#elif defined(_WIN32)
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, flag, sizeof on) == 0;
#else
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, flag, sizeof on) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, flag, sizeof on) == 0;
#endif
}

Socket receiveSocket(const std::string& address, uint16_t port, unsigned index, std::string& error)
{
    const Socket fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == kNoSocket) {
        error = "socket: " + socketErrorText(socketError());
        return kNoSocket;
    }
    sockaddr_in at = ipv4Endpoint({}, port);
    const bool ok = inet_pton(AF_INET, address.c_str(), &at.sin_addr) == 1 && shareAndBind(fd, index) &&
                    bind(fd, reinterpret_cast<const sockaddr*>(&at), sizeof at) == 0;
    if (!ok) {
        error = "port " + std::to_string(port) + ": " + socketErrorText(socketError());
        closeSocket(fd);
        return kNoSocket;
    }
    return fd;
}

} // namespace

Receiver::Receiver(Options options, OnDatagram onDatagram)
    : options_(std::move(options)), onDatagram_(std::move(onDatagram))
{
    startSockets();
    open();
    thread_ = std::thread([this] { run(); });
}

Receiver::~Receiver()
{
    running_ = false;
    thread_.join();
    close();
}

std::string Receiver::error() const
{
    std::lock_guard lock(errorMutex_);
    return error_;
}

void Receiver::setError(std::string error)
{
    std::lock_guard lock(errorMutex_);
    error_ = std::move(error);
}

bool Receiver::open()
{
    index_ = if_nametoindex(options_.interface.c_str());
    if (index_ == 0) {
        setError("no interface " + options_.interface);
        return false;
    }
    std::string error;
    for (size_t i = 0; i < sockets_.size(); ++i) {
        sockets_[i] = receiveSocket(options_.address, options_.ports[i], index_, error);
        if (sockets_[i] == kNoSocket) {
            setError(error);
            close();
            return false;
        }
    }
    setError({});
    listening_ = true;
    return true;
}

void Receiver::close()
{
    listening_ = false;
    for (Socket& fd : sockets_) {
        if (fd != kNoSocket)
            closeSocket(fd);
        fd = kNoSocket;
    }
}

void Receiver::run()
{
    using Clock = std::chrono::steady_clock;
    const auto retry = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(kRetry));
    auto checked = Clock::now();
    uint8_t buffer[2048];
    while (running_) {
        if (Clock::now() - checked >= retry) {
            checked = Clock::now();
            if (listening_ && if_nametoindex(options_.interface.c_str()) != index_) {
                close();
                setError("interface " + options_.interface + " went away");
            }
            if (!listening_)
                open();
        }
        if (!listening_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
            continue;
        }
        pollfd fds[3];
        for (size_t i = 0; i < 3; ++i)
            fds[i] = {sockets_[i], POLLIN, 0};
        if (pollSockets(fds, 3, kPollMs) <= 0)
            continue;
        for (size_t i = 0; i < 3; ++i) {
            if (!(fds[i].revents & POLLIN))
                continue;
            const long n = receiveSome(sockets_[i], buffer, sizeof buffer);
            if (n <= 0)
                continue;
            ++datagrams_;
            onDatagram_(uint16_t(kAnnouncePort + i), buffer, size_t(n));
        }
    }
}

} // namespace azl::djlink
