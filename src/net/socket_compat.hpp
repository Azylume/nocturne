#pragma once

// Plain sockets on POSIX and Winsock behind one set of names, for the code that talks to sockets directly.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstddef>
#include <cstdint>
#include <string>

namespace azl {

#ifdef _WIN32
using Socket = SOCKET;
inline const Socket kNoSocket = INVALID_SOCKET;
#else
using Socket = int;
inline constexpr Socket kNoSocket = -1;
#endif

// Starts Winsock once per process before the first socket call; does nothing elsewhere. Safe from any thread.
void startSockets();

void closeSocket(Socket socket);
// The last socket call's error on this thread: errno, or WSAGetLastError().
int socketError();
std::string socketErrorText(int error);
// What a getaddrinfo error means.
std::string resolveErrorText(int error);
// The error means a non-blocking call would have waited, or a non-blocking connect is under way.
bool wouldBlock(int error);

bool setNonBlocking(Socket socket);
// Not handed to child processes.
void noInherit(Socket socket);
// Receive and send calls give up after `seconds`.
void setTimeouts(Socket socket, double seconds);

// A send on this socket after the peer closed fails instead of raising SIGPIPE (macOS; elsewhere sends never raise it).
void noSigPipe(Socket socket);
// send and recv, never raising SIGPIPE once noSigPipe was called: the byte count, negative on an error.
long sendSome(Socket socket, const void* data, size_t size);
long receiveSome(Socket socket, void* into, size_t size);
// Sends all of `size` bytes; false when the connection fails first.
bool sendAll(Socket socket, const void* data, size_t size);

int pollSockets(pollfd* fds, size_t count, int timeoutMs);

// An IPv4 endpoint ready for bind or connect.
sockaddr_in ipv4Endpoint(in_addr address, uint16_t port);

// Wakes a thread blocked in pollSockets: its socket becomes readable after signal() until drain().
class Wake {
public:
    Wake();
    ~Wake();
    Wake(const Wake&) = delete;
    Wake& operator=(const Wake&) = delete;

    Socket socket() const { return read_; }
    void signal();
    void drain();

private:
    Socket read_ = kNoSocket;
    Socket write_ = kNoSocket;
};

} // namespace azl
