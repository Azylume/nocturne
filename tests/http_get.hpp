#pragma once

#include "net/socket_compat.hpp"

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
    azl::startSockets();
    const azl::Socket fd = ::socket(AF_INET, SOCK_STREAM, 0);
    in_addr loopback{};
    loopback.s_addr = htonl(INADDR_LOOPBACK);
    const sockaddr_in at = azl::ipv4Endpoint(loopback, port);
    azl::setTimeouts(fd, 10.0);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&at), sizeof at) != 0) {
        azl::closeSocket(fd);
        return {};
    }
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    azl::sendAll(fd, request.data(), request.size());
    std::string reply;
    char buffer[4096];
    for (long n; (n = azl::receiveSome(fd, buffer, sizeof buffer)) > 0;)
        reply.append(buffer, size_t(n));
    azl::closeSocket(fd);
    const size_t space = reply.find(' ');
    const size_t body = reply.find("\r\n\r\n");
    if (space == std::string::npos || body == std::string::npos)
        return {};
    return {std::atoi(reply.c_str() + space + 1), reply.substr(body + 4)};
}

} // namespace
