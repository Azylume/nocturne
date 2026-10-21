#pragma once

#include <map>
#include <stdexcept>
#include <string>

namespace azl {

struct HttpRequest {
    std::string method;
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers; // lowercase names
    std::string body;
};

// The connection failed or dropped before a whole response arrived.
struct TransportError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A blocking HTTP(S) request over NSURLSession (http.mm). Throws TransportError.
HttpResponse httpRequest(const HttpRequest& request);

} // namespace azl
