#include "djlink/djlink_receiver.hpp"

#include "djlink/djlink_packets.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace azl::djlink {

namespace {

constexpr int kPollMs = 250;

int receiveSocket(const std::string& address, uint16_t port, unsigned index, std::string& error)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return -1;
    }
    const int on = 1;
    sockaddr_in at{};
    at.sin_len = sizeof at;
    at.sin_family = AF_INET;
    at.sin_port = htons(port);
    const bool ok = inet_pton(AF_INET, address.c_str(), &at.sin_addr) == 1 &&
                    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) == 0 &&
                    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on) == 0 &&
                    setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &index, sizeof index) == 0 &&
                    bind(fd, reinterpret_cast<const sockaddr*>(&at), sizeof at) == 0;
    if (!ok) {
        error = "port " + std::to_string(port) + ": " + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

Receiver::Receiver(Options options, OnDatagram onDatagram)
    : options_(std::move(options)), onDatagram_(std::move(onDatagram))
{
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
        if (sockets_[i] < 0) {
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
    for (int& fd : sockets_) {
        if (fd >= 0)
            ::close(fd);
        fd = -1;
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
        if (poll(fds, 3, kPollMs) <= 0)
            continue;
        for (size_t i = 0; i < 3; ++i) {
            if (!(fds[i].revents & POLLIN))
                continue;
            const ssize_t n = recv(sockets_[i], buffer, sizeof buffer, 0);
            if (n <= 0)
                continue;
            ++datagrams_;
            onDatagram_(uint16_t(kAnnouncePort + i), buffer, size_t(n));
        }
    }
}

} // namespace azl::djlink
