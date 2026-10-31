#include "net/socket_compat.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/time.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace azl {

#ifdef _WIN32

void startSockets()
{
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!started)
        throw std::runtime_error("cannot start Winsock");
}

void closeSocket(Socket socket)
{
    ::closesocket(socket);
}

int socketError()
{
    return WSAGetLastError();
}

std::string socketErrorText(int error)
{
    return std::system_category().message(error);
}

std::string resolveErrorText(int error)
{
    return gai_strerrorA(error);
}

bool wouldBlock(int error)
{
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}

bool setNonBlocking(Socket socket)
{
    u_long on = 1;
    return ::ioctlsocket(socket, FIONBIO, &on) == 0;
}

void noInherit(Socket socket)
{
    SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0);
}

void setTimeouts(Socket socket, double seconds)
{
    const DWORD ms = DWORD(std::lround(seconds * 1000.0));
    ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
    ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
}

long sendSome(Socket socket, const void* data, size_t size)
{
    return ::send(socket, static_cast<const char*>(data), int(std::min<size_t>(size, 1 << 30)), 0);
}

long receiveSome(Socket socket, void* into, size_t size)
{
    return ::recv(socket, static_cast<char*>(into), int(std::min<size_t>(size, 1 << 30)), 0);
}

int pollSockets(pollfd* fds, size_t count, int timeoutMs)
{
    return ::WSAPoll(fds, ULONG(count), timeoutMs);
}

// Winsock polls sockets only, so the wake is a loopback UDP socket that sends to itself.
Wake::Wake()
{
    startSockets();
    read_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in at = ipv4Endpoint({}, 0);
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = sizeof at;
    if (read_ == kNoSocket || ::bind(read_, reinterpret_cast<sockaddr*>(&at), size) != 0 ||
        ::getsockname(read_, reinterpret_cast<sockaddr*>(&at), &size) != 0 ||
        ::connect(read_, reinterpret_cast<sockaddr*>(&at), size) != 0 || !setNonBlocking(read_)) {
        closeSocket(read_);
        throw std::runtime_error("cannot create a wake socket");
    }
    write_ = read_;
}

Wake::~Wake()
{
    closeSocket(read_);
}

void Wake::signal()
{
    const char byte = 1;
    ::send(write_, &byte, 1, 0);
}

void Wake::drain()
{
    char buffer[64];
    while (::recv(read_, buffer, sizeof buffer, 0) > 0) {
    }
}

void noSigPipe(Socket) {}

#else

void startSockets() {}

void closeSocket(Socket socket)
{
    ::close(socket);
}

int socketError()
{
    return errno;
}

std::string socketErrorText(int error)
{
    return std::strerror(error);
}

std::string resolveErrorText(int error)
{
    return gai_strerror(error);
}

bool wouldBlock(int error)
{
    return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS;
}

bool setNonBlocking(Socket socket)
{
    const int flags = ::fcntl(socket, F_GETFL, 0);
    return flags >= 0 && ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

void noInherit(Socket socket)
{
    ::fcntl(socket, F_SETFD, FD_CLOEXEC);
}

void setTimeouts(Socket socket, double seconds)
{
    const timeval timeout{time_t(seconds), int((seconds - double(time_t(seconds))) * 1e6)};
    ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
}

#ifdef __APPLE__
constexpr int kSendFlags = 0;
#else
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

void noSigPipe(Socket socket)
{
#ifdef __APPLE__
    const int on = 1;
    ::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
}

long sendSome(Socket socket, const void* data, size_t size)
{
    return ::send(socket, data, size, kSendFlags);
}

long receiveSome(Socket socket, void* into, size_t size)
{
    return ::recv(socket, into, size, 0);
}

int pollSockets(pollfd* fds, size_t count, int timeoutMs)
{
    return ::poll(fds, nfds_t(count), timeoutMs);
}

Wake::Wake()
{
    int fds[2];
    if (::pipe(fds) != 0)
        throw std::runtime_error("cannot create a wake pipe");
    read_ = fds[0];
    write_ = fds[1];
    setNonBlocking(read_);
    setNonBlocking(write_);
    noInherit(read_);
    noInherit(write_);
}

Wake::~Wake()
{
    ::close(read_);
    ::close(write_);
}

void Wake::signal()
{
    const char byte = 1;
    [[maybe_unused]] const ssize_t n = ::write(write_, &byte, 1);
}

void Wake::drain()
{
    char buffer[64];
    while (::read(read_, buffer, sizeof buffer) > 0) {
    }
}

#endif

bool sendAll(Socket socket, const void* data, size_t size)
{
    const auto* bytes = static_cast<const char*>(data);
    for (size_t sent = 0; sent < size;) {
        const long n = sendSome(socket, bytes + sent, size - sent);
        if (n <= 0)
            return false;
        sent += size_t(n);
    }
    return true;
}

sockaddr_in ipv4Endpoint(in_addr address, uint16_t port)
{
    sockaddr_in at{};
#ifdef __APPLE__
    at.sin_len = sizeof at;
#endif
    at.sin_family = AF_INET;
    at.sin_port = htons(port);
    at.sin_addr = address;
    return at;
}

} // namespace azl
