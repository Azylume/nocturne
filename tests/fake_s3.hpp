#pragma once

#include "upload/s3.hpp"

#include <doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string readAll(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void writeAll(const std::filesystem::path& file, const std::string& data)
{
    std::ofstream(file, std::ios::binary) << data;
}

std::string bytes(size_t size)
{
    std::string data(size, '\0');
    for (size_t i = 0; i < size; ++i)
        data[i] = char((i * 131 + i / 7) & 0xff);
    return data;
}

// An S3-compatible object store on a loopback port, enough for the upload and the venue's download: HEAD, PUT,
// multipart create, part, list and complete, and ranged GET, with SigV4 checked against its one key pair. One request
// per connection.
class FakeS3 {
public:
    static constexpr const char* kBucket = "media";
    static constexpr const char* kKeyId = "test-key-id";
    static constexpr const char* kSecret = "test-secret";

    FakeS3()
    {
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
        socklen_t length = sizeof address;
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        REQUIRE(listen(listener_, 16) == 0);
        thread_ = std::thread([this] { serve(); });
    }

    ~FakeS3()
    {
        stop_ = true;
        thread_.join();
        close(listener_);
    }

    std::string endpoint() const { return "http://127.0.0.1:" + std::to_string(port_); }

    azl::S3Config config(const std::string& secret = kSecret, const std::string& keyId = kKeyId) const
    {
        return {endpoint(), kBucket, keyId, secret};
    }

    std::map<std::string, std::string> objects()
    {
        std::lock_guard lock(mutex_);
        return objects_;
    }

    int count(const std::string& call)
    {
        std::lock_guard lock(mutex_);
        return calls_[call];
    }

    // After `parts` more parts the connection is gone: every request drops until restore().
    void dropAfterParts(int parts)
    {
        std::lock_guard lock(mutex_);
        partsUntilDrop_ = parts;
    }

    // After `gets` more object GETs the connection is gone: every GET drops until restore().
    void dropAfterGets(int gets)
    {
        std::lock_guard lock(mutex_);
        getsUntilDrop_ = gets;
    }

    void restore()
    {
        std::lock_guard lock(mutex_);
        partsUntilDrop_ = -1;
        getsUntilDrop_ = -1;
    }

    void putObject(const std::string& key, const std::string& data)
    {
        std::lock_guard lock(mutex_);
        objects_[key] = data;
    }

    // The next `times` downloads of the object from its start get its first byte flipped.
    void corrupt(const std::string& key, int times)
    {
        std::lock_guard lock(mutex_);
        corruptions_[key] = times;
    }

    // The Range header of every object GET, in order.
    std::vector<std::string> ranges()
    {
        std::lock_guard lock(mutex_);
        return ranges_;
    }

private:
    struct Request {
        std::string method, path, query, body;
        std::map<std::string, std::string> headers;
    };

    struct Reply {
        int status = 200;
        std::string body;
        std::map<std::string, std::string> headers;
    };

    void serve()
    {
        while (!stop_) {
            pollfd p{listener_, POLLIN, 0};
            if (poll(&p, 1, 20) <= 0)
                continue;
            const int fd = accept(listener_, nullptr, nullptr);
            if (fd < 0)
                continue;
            if (const auto request = read(fd)) {
                if (const auto reply = handle(*request))
                    write(fd, *request, *reply);
            }
            close(fd);
        }
    }

    static std::optional<Request> read(int fd)
    {
        std::string data;
        char buffer[65536];
        size_t headerEnd;
        while ((headerEnd = data.find("\r\n\r\n")) == std::string::npos) {
            const ssize_t n = recv(fd, buffer, sizeof buffer, 0);
            if (n <= 0)
                return std::nullopt;
            data.append(buffer, size_t(n));
        }
        Request r;
        std::istringstream head(data.substr(0, headerEnd));
        std::string line, target;
        std::getline(head, line);
        std::istringstream(line) >> r.method >> target;
        r.path = target.substr(0, target.find('?'));
        r.query = target.find('?') == std::string::npos ? "" : target.substr(target.find('?') + 1);
        while (std::getline(head, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const size_t colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            std::string name = line.substr(0, colon);
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            r.headers[name] = line.substr(line.find_first_not_of(' ', colon + 1));
        }
        const size_t length = r.headers.count("content-length") ? std::stoul(r.headers["content-length"]) : 0;
        r.body = data.substr(headerEnd + 4);
        while (r.body.size() < length) {
            const ssize_t n = recv(fd, buffer, sizeof buffer, 0);
            if (n <= 0)
                return std::nullopt;
            r.body.append(buffer, size_t(n));
        }
        return r;
    }

    static void write(int fd, const Request& request, const Reply& reply)
    {
        std::string out = "HTTP/1.1 " + std::to_string(reply.status) + " X\r\nConnection: close\r\n";
        for (const auto& [name, value] : reply.headers)
            out += name + ": " + value + "\r\n";
        if (!reply.headers.count("Content-Length"))
            out += "Content-Length: " + std::to_string(reply.body.size()) + "\r\n";
        out += "\r\n";
        if (request.method != "HEAD")
            out += reply.body;
        for (size_t sent = 0; sent < out.size();) {
            const ssize_t n = send(fd, out.data() + sent, out.size() - sent, 0);
            if (n <= 0)
                return;
            sent += size_t(n);
        }
    }

    static Reply error(int status, const std::string& code)
    {
        return {status, "<?xml version=\"1.0\"?><Error><Code>" + code + "</Code><Message>fake</Message></Error>", {}};
    }

    static std::map<std::string, std::string> params(const std::string& query)
    {
        std::map<std::string, std::string> out;
        std::istringstream in(query);
        for (std::string pair; std::getline(in, pair, '&');) {
            const size_t eq = pair.find('=');
            out[pair.substr(0, eq)] = eq == std::string::npos ? "" : pair.substr(eq + 1);
        }
        return out;
    }

    std::optional<Reply> checkSignature(const Request& r)
    {
        const auto auth = r.headers.find("authorization");
        if (auth == r.headers.end())
            return error(403, "AccessDenied");
        const std::string& a = auth->second;
        const size_t credential = a.find("Credential=") + 11;
        if (a.substr(credential, a.find('/', credential) - credential) != kKeyId)
            return error(403, "InvalidAccessKeyId");
        if (r.headers.at("x-amz-content-sha256") != azl::sha256Hex(r.body))
            return error(400, "XAmzContentSHA256Mismatch");
        const size_t names = a.find("SignedHeaders=") + 14;
        std::map<std::string, std::string> signedHeaders;
        std::istringstream list(a.substr(names, a.find(',', names) - names));
        for (std::string name; std::getline(list, name, ';');)
            signedHeaders[name] = r.headers.count(name) ? r.headers.at(name) : "";
        std::string query;
        for (const auto& [key, value] : params(r.query))
            query += (query.empty() ? "" : "&") + key + "=" + value;
        const std::string amzDate = r.headers.count("x-amz-date") ? r.headers.at("x-amz-date") : "";
        const std::string expected = azl::signV4(r.method, r.path, query, signedHeaders,
                                                 r.headers.at("x-amz-content-sha256"), amzDate, "auto", kKeyId, kSecret);
        if (expected != a)
            return error(403, "SignatureDoesNotMatch");
        return std::nullopt;
    }

    // Nothing when the connection drops instead.
    std::optional<Reply> handle(const Request& r)
    {
        std::lock_guard lock(mutex_);
        auto q = params(r.query);
        const bool part = r.method == "PUT" && q.count("partNumber");
        const bool get = r.method == "GET" && !q.count("uploadId");
        if (partsUntilDrop_ == 0 || (get && getsUntilDrop_ == 0))
            return std::nullopt;
        if (get && getsUntilDrop_ > 0)
            getsUntilDrop_--;
        if (part && partsUntilDrop_ > 0)
            partsUntilDrop_--;
        if (const auto refused = checkSignature(r))
            return refused;
        const std::string prefix = std::string("/") + kBucket + "/";
        if (r.path.rfind(prefix, 0) != 0)
            return error(404, "NoSuchBucket");
        const std::string key = r.path.substr(prefix.size());

        if (r.method == "HEAD") {
            calls_["head"]++;
            const auto it = objects_.find(key);
            if (it == objects_.end())
                return Reply{404, {}, {{"Content-Length", "0"}}};
            return Reply{200, {}, {{"Content-Length", std::to_string(it->second.size())}}};
        }
        if (get)
            return object(key, r.headers.count("range") ? r.headers.at("range") : "");
        if (r.method == "PUT" && !part) {
            calls_["put"]++;
            objects_[key] = r.body;
            return Reply{200, {}, {{"ETag", "\"whole\""}}};
        }
        if (r.method == "POST" && q.count("uploads")) {
            calls_["create"]++;
            const std::string id = "upload+" + std::to_string(++nextUpload_) + "/=";
            uploads_[id] = {};
            return Reply{200, "<InitiateMultipartUploadResult><Bucket>media</Bucket><Key>" + key +
                                  "</Key><UploadId>" + id + "</UploadId></InitiateMultipartUploadResult>", {}};
        }
        const std::string id = decode(q["uploadId"]);
        const auto upload = uploads_.find(id);
        if (upload == uploads_.end())
            return error(404, "NoSuchUpload");
        if (part) {
            calls_["part"]++;
            upload->second[std::stoi(q["partNumber"])] = r.body;
            return Reply{200, {}, {{"ETag", "\"" + azl::sha256Hex(r.body).substr(0, 32) + "\""}}};
        }
        if (r.method == "GET") {
            calls_["list"]++;
            std::string body = "<ListPartsResult><IsTruncated>false</IsTruncated>";
            for (const auto& [number, data] : upload->second)
                body += "<Part><PartNumber>" + std::to_string(number) + "</PartNumber><ETag>&quot;" +
                        azl::sha256Hex(data).substr(0, 32) + "&quot;</ETag><Size>" + std::to_string(data.size()) +
                        "</Size></Part>";
            return Reply{200, body + "</ListPartsResult>", {}};
        }
        if (r.method == "POST") {
            calls_["complete"]++;
            const auto numbers = azl::xmlValues(r.body, "PartNumber");
            const auto etags = azl::xmlValues(r.body, "ETag");
            std::string object;
            for (size_t i = 0; i < numbers.size(); ++i) {
                const auto data = upload->second.find(std::stoi(numbers[i]));
                if (data == upload->second.end() ||
                    etags.at(i) != "\"" + azl::sha256Hex(data->second).substr(0, 32) + "\"")
                    return error(400, "InvalidPart");
                object += data->second;
            }
            objects_[key] = object;
            uploads_.erase(upload);
            return Reply{200, "<CompleteMultipartUploadResult><Key>" + key + "</Key></CompleteMultipartUploadResult>", {}};
        }
        return error(400, "NotImplemented");
    }

    Reply object(const std::string& key, const std::string& range)
    {
        calls_["get"]++;
        ranges_.push_back(range);
        const auto it = objects_.find(key);
        if (it == objects_.end())
            return error(404, "NoSuchKey");
        unsigned long long first = 0, last = it->second.size() - 1;
        if (!range.empty() && std::sscanf(range.c_str(), "bytes=%llu-%llu", &first, &last) < 1)
            return error(400, "InvalidRange");
        last = std::min<unsigned long long>(last, it->second.size() - 1);
        if (first == 0)
            corrupting_[key] = corruptions_[key]-- > 0;
        std::string data = it->second;
        if (corrupting_[key])
            data[0] = char(~data[0]);
        if (range.empty())
            return Reply{200, data, {}};
        return Reply{206, data.substr(first, last - first + 1),
                     {{"Content-Range", "bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" +
                                            std::to_string(data.size())}}};
    }

    static std::string decode(const std::string& text)
    {
        std::string out;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '%' && i + 2 < text.size()) {
                out += char(std::stoi(text.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else {
                out += text[i];
            }
        }
        return out;
    }

    int listener_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::map<std::string, std::string> objects_;
    std::map<std::string, std::map<int, std::string>> uploads_;
    std::map<std::string, int> calls_;
    int nextUpload_ = 0;
    int partsUntilDrop_ = -1;
    int getsUntilDrop_ = -1;
    std::map<std::string, int> corruptions_;
    std::map<std::string, bool> corrupting_;
    std::vector<std::string> ranges_;
};

} // namespace
