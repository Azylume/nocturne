// httpRequest against a one-shot local server: NSURLSession on macOS, WinHTTP on Windows, the same answers.

#include "net/socket_compat.hpp"
#include "upload/http.hpp"

#include <doctest.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <thread>

namespace {

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

// Answers one request on a loopback port with `reply`, and keeps what it was sent.
class OneShotServer {
public:
    explicit OneShotServer(std::string reply) : reply_(std::move(reply))
    {
        azl::startSockets();
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        in_addr loopback{};
        loopback.s_addr = htonl(INADDR_LOOPBACK);
        sockaddr_in at = azl::ipv4Endpoint(loopback, 0);
        REQUIRE(::bind(listener_, reinterpret_cast<sockaddr*>(&at), sizeof at) == 0);
        REQUIRE(::listen(listener_, 1) == 0);
        socklen_t size = sizeof at;
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&at), &size);
        port = ntohs(at.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~OneShotServer()
    {
        if (thread_.joinable())
            thread_.join();
        azl::closeSocket(listener_);
    }

    // What the server was sent, once it has answered.
    std::string received()
    {
        thread_.join();
        return request_;
    }

    uint16_t port = 0;

private:
    void serve()
    {
        const azl::Socket client = ::accept(listener_, nullptr, nullptr);
        if (client == azl::kNoSocket)
            return;
        azl::setTimeouts(client, 5.0);
        char buffer[4096];
        while (request_.find("\r\n\r\n") == std::string::npos || !bodyIn()) {
            const long n = azl::receiveSome(client, buffer, sizeof buffer);
            if (n <= 0)
                break;
            request_.append(buffer, size_t(n));
        }
        azl::sendAll(client, reply_.data(), reply_.size());
        azl::closeSocket(client);
    }

    bool bodyIn() const
    {
        const size_t end = request_.find("\r\n\r\n") + 4;
        const size_t length = lower(request_).find("content-length: ");
        return length == std::string::npos || request_.size() - end >= std::stoul(request_.substr(length + 16));
    }

    std::string reply_;
    std::string request_;
    azl::Socket listener_ = azl::kNoSocket;
    std::thread thread_;
};

} // namespace

TEST_CASE("http: a request goes out with its method, headers and body, and the answer comes back whole")
{
    OneShotServer server("HTTP/1.1 201 Created\r\nContent-Length: 5\r\nX-Amz-Request-Id: abc\r\nConnection: close\r\n\r\n"
                         "hello");
    const azl::HttpResponse response = azl::httpRequest({"PUT", "http://127.0.0.1:" + std::to_string(server.port) +
                                                                    "/bucket/key?uploads=1",
                                                         {{"x-amz-date", "20261031T000000Z"}, {"host", "ignored"}},
                                                         "payload"});
    CHECK(response.status == 201);
    CHECK(response.body == "hello");
    CHECK(response.headers.at("x-amz-request-id") == "abc");
    CHECK(response.headers.at("content-length") == "5");
    const std::string request = server.received();
    const std::string sent = lower(request);
    CHECK(request.starts_with("PUT /bucket/key?uploads=1 HTTP/1.1\r\n"));
    CHECK(sent.find("\r\nhost: 127.0.0.1:" + std::to_string(server.port) + "\r\n") != std::string::npos);
    CHECK(sent.find("\r\nx-amz-date: 20261031t000000z\r\n") != std::string::npos);
    CHECK(request.ends_with("\r\n\r\npayload"));
}

TEST_CASE("http: a connection that fails throws a transport error naming the request")
{
    uint16_t closed = 0;
    {
        OneShotServer server("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
        closed = server.port;
        azl::httpRequest({"GET", "http://127.0.0.1:" + std::to_string(closed) + "/", {}, ""});
    }
    const std::string url = "http://127.0.0.1:" + std::to_string(closed) + "/";
    try {
        azl::httpRequest({"GET", url, {}, ""});
        FAIL("no transport error");
    } catch (const azl::TransportError& e) {
        CHECK(std::string(e.what()).starts_with("GET " + url + ": "));
    }
}
