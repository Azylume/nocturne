#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

struct HttpReply {
    // 0 when nothing answered.
    int status = 0;
    std::string body;
};

// One GET to 127.0.0.1:port with the given Host header.
[[maybe_unused]] HttpReply httpGet(uint16_t port, const std::string& path, const std::string& host = "127.0.0.1")
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in at{};
    at.sin_len = sizeof at;
    at.sin_family = AF_INET;
    at.sin_port = htons(port);
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const timeval timeout{10, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&at), sizeof at) != 0) {
        ::close(fd);
        return {};
    }
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    ::send(fd, request.data(), request.size(), 0);
    std::string reply;
    char buffer[4096];
    for (ssize_t n; (n = ::recv(fd, buffer, sizeof buffer, 0)) > 0;)
        reply.append(buffer, size_t(n));
    ::close(fd);
    const size_t space = reply.find(' ');
    const size_t body = reply.find("\r\n\r\n");
    if (space == std::string::npos || body == std::string::npos)
        return {};
    return {std::atoi(reply.c_str() + space + 1), reply.substr(body + 4)};
}

} // namespace
