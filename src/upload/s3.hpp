#pragma once

#include "upload/http.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace azl {

struct S3Error : std::runtime_error {
    S3Error(int status, std::string code, const std::string& what);
    int status;
    std::string code;
};

struct S3Config {
    std::string endpoint; // https://ACCOUNT.r2.cloudflarestorage.com
    std::string bucket;
    std::string accessKeyId;
    std::string secretAccessKey;
    std::string region = "auto";
};

struct S3Part {
    int number = 0;
    std::string etag;
    uint64_t size = 0;
};

std::string sha256Hex(const std::string& data);
// RFC 3986 encoding as SigV4 wants it: everything but unreserved characters (and '/' when kept) is %XX.
std::string uriEncode(const std::string& text, bool keepSlash);
// The Authorization header of an AWS Signature Version 4 request that signs every one of `headers` (lowercase names).
// `query` is the canonical query string.
std::string signV4(const std::string& method, const std::string& path, const std::string& query,
                   const std::map<std::string, std::string>& headers, const std::string& payloadHash,
                   const std::string& amzDate, const std::string& region, const std::string& accessKeyId,
                   const std::string& secretAccessKey);
// The text of every <tag>...</tag> in an S3 XML reply, entities decoded.
std::vector<std::string> xmlValues(const std::string& xml, const std::string& tag);

// The S3 calls a content-addressed upload needs, path-style (bucket in the path), as R2 and MinIO take them. Throws
// S3Error for an error reply and TransportError when the connection drops.
class S3Client {
public:
    using Transport = std::function<HttpResponse(const HttpRequest&)>;

    explicit S3Client(S3Config config, Transport transport = httpRequest);

    // The object's size, or nothing when there is no such object.
    std::optional<uint64_t> head(const std::string& key);
    // `length` bytes of the object from `offset` on (fewer at its end), or nothing when there is no such object.
    std::optional<std::string> getRange(const std::string& key, uint64_t offset, uint64_t length);
    void put(const std::string& key, const std::string& body, const std::string& contentType);
    // Returns the upload id.
    std::string createMultipart(const std::string& key, const std::string& contentType);
    // Returns the part's ETag.
    std::string uploadPart(const std::string& key, const std::string& uploadId, int number, const std::string& body);
    // The parts uploaded so far, or nothing when the upload is gone (completed, aborted or expired).
    std::optional<std::vector<S3Part>> listParts(const std::string& key, const std::string& uploadId);
    void completeMultipart(const std::string& key, const std::string& uploadId, const std::vector<S3Part>& parts);

    const S3Config& config() const { return config_; }

private:
    HttpResponse send(const std::string& method, const std::string& key, const std::map<std::string, std::string>& query,
                      const std::string& body, std::map<std::string, std::string> headers = {});

    S3Config config_;
    Transport transport_;
};

} // namespace azl
