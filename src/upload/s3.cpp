#include "upload/s3.hpp"

#include "platform/crypto.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <utility>

namespace azl {

namespace {

std::string hex(const unsigned char* bytes, size_t size)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        out += kHex[bytes[i] >> 4];
        out += kHex[bytes[i] & 15];
    }
    return out;
}

std::string hmac(const std::string& key, const std::string& data)
{
    const Sha256Digest digest = hmacSha256(key.data(), key.size(), data.data(), data.size());
    return {reinterpret_cast<const char*>(digest.data()), digest.size()};
}

std::string utcStamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char text[17];
    std::strftime(text, sizeof text, "%Y%m%dT%H%M%SZ", &tm);
    return text;
}

// host[:port] of an endpoint URL.
std::string authority(const std::string& endpoint)
{
    const size_t scheme = endpoint.find("://");
    const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    return endpoint.substr(start, endpoint.find('/', start) - start);
}

std::string canonicalQuery(const std::map<std::string, std::string>& query)
{
    std::string out;
    for (const auto& [key, value] : query)
        out += (out.empty() ? "" : "&") + uriEncode(key, false) + "=" + uriEncode(value, false);
    return out;
}

std::string decodeEntities(std::string text)
{
    static const std::pair<const char*, const char*> kEntities[] = {
        {"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&#34;", "\""}, {"&amp;", "&"}};
    for (const auto& [entity, plain] : kEntities)
        for (size_t at = text.find(entity); at != std::string::npos; at = text.find(entity, at + 1))
            text.replace(at, std::char_traits<char>::length(entity), plain);
    return text;
}

std::string escapeXml(const std::string& text)
{
    std::string out;
    for (char c : text) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

S3Error errorOf(const std::string& what, const HttpResponse& response)
{
    const auto codes = xmlValues(response.body, "Code");
    const auto messages = xmlValues(response.body, "Message");
    const std::string code = codes.empty() ? "" : codes.front();
    std::string text = what + ": HTTP " + std::to_string(response.status);
    if (!code.empty())
        text += " " + code;
    if (!messages.empty())
        text += ": " + messages.front();
    return S3Error(response.status, code, text);
}

} // namespace

S3Error::S3Error(int status, std::string code, const std::string& what)
    : std::runtime_error(what), status(status), code(std::move(code))
{
}

std::string sha256Hex(const std::string& data)
{
    const Sha256Digest digest = sha256(data.data(), data.size());
    return hex(digest.data(), digest.size());
}

std::string uriEncode(const std::string& text, bool keepSlash)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (keepSlash && c == '/')) {
            out += char(c);
            continue;
        }
        out += '%';
        out += kHex[c >> 4];
        out += kHex[c & 15];
    }
    return out;
}

std::string signV4(const std::string& method, const std::string& path, const std::string& query,
                   const std::map<std::string, std::string>& headers, const std::string& payloadHash,
                   const std::string& amzDate, const std::string& region, const std::string& accessKeyId,
                   const std::string& secretAccessKey)
{
    std::string canonicalHeaders, signedHeaders;
    for (const auto& [name, value] : headers) {
        canonicalHeaders += name + ":" + value + "\n";
        signedHeaders += (signedHeaders.empty() ? "" : ";") + name;
    }
    const std::string canonical =
        method + "\n" + path + "\n" + query + "\n" + canonicalHeaders + "\n" + signedHeaders + "\n" + payloadHash;
    const std::string date = amzDate.substr(0, 8);
    const std::string scope = date + "/" + region + "/s3/aws4_request";
    const std::string toSign = "AWS4-HMAC-SHA256\n" + amzDate + "\n" + scope + "\n" + sha256Hex(canonical);
    const std::string key = hmac(hmac(hmac(hmac("AWS4" + secretAccessKey, date), region), "s3"), "aws4_request");
    const std::string signature = hmac(key, toSign);
    return "AWS4-HMAC-SHA256 Credential=" + accessKeyId + "/" + scope + ", SignedHeaders=" + signedHeaders +
           ", Signature=" + hex(reinterpret_cast<const unsigned char*>(signature.data()), signature.size());
}

std::vector<std::string> xmlValues(const std::string& xml, const std::string& tag)
{
    std::vector<std::string> values;
    const std::string open = "<" + tag + ">", close = "</" + tag + ">";
    for (size_t at = xml.find(open); at != std::string::npos; at = xml.find(open, at)) {
        at += open.size();
        const size_t end = xml.find(close, at);
        if (end == std::string::npos)
            break;
        values.push_back(decodeEntities(xml.substr(at, end - at)));
        at = end + close.size();
    }
    return values;
}

S3Client::S3Client(S3Config config, Transport transport) : config_(std::move(config)), transport_(std::move(transport))
{
    while (!config_.endpoint.empty() && config_.endpoint.back() == '/')
        config_.endpoint.pop_back();
}

HttpResponse S3Client::send(const std::string& method, const std::string& key,
                            const std::map<std::string, std::string>& query, const std::string& body,
                            std::map<std::string, std::string> headers)
{
    const std::string payloadHash = sha256Hex(body);
    headers["host"] = authority(config_.endpoint);
    headers["x-amz-content-sha256"] = payloadHash;
    headers["x-amz-date"] = utcStamp();
    const std::string path = "/" + uriEncode(config_.bucket, false) + "/" + uriEncode(key, true);
    const std::string q = canonicalQuery(query);
    const std::string authorization = signV4(method, path, q, headers, payloadHash, headers["x-amz-date"],
                                             config_.region, config_.accessKeyId, config_.secretAccessKey);
    headers["authorization"] = authorization;
    return transport_({method, config_.endpoint + path + (q.empty() ? "" : "?" + q), std::move(headers), body});
}

std::optional<uint64_t> S3Client::head(const std::string& key)
{
    const HttpResponse response = send("HEAD", key, {}, {});
    if (response.status == 404)
        return std::nullopt;
    if (response.status != 200)
        throw errorOf("HEAD " + key, response);
    const auto length = response.headers.find("content-length");
    return length == response.headers.end() ? 0 : std::stoull(length->second);
}

std::optional<std::string> S3Client::getRange(const std::string& key, uint64_t offset, uint64_t length)
{
    const std::string range = "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + length - 1);
    HttpResponse response = send("GET", key, {}, {}, {{"range", range}});
    if (response.status == 404)
        return std::nullopt;
    if (response.status == 200)
        return response.body.substr(std::min<uint64_t>(offset, response.body.size()), length);
    if (response.status != 206)
        throw errorOf("GET " + key + " " + range, response);
    return std::move(response.body);
}

void S3Client::put(const std::string& key, const std::string& body, const std::string& contentType)
{
    const HttpResponse response = send("PUT", key, {}, body, {{"content-type", contentType}});
    if (response.status != 200)
        throw errorOf("PUT " + key, response);
}

std::string S3Client::createMultipart(const std::string& key, const std::string& contentType)
{
    const HttpResponse response = send("POST", key, {{"uploads", ""}}, {}, {{"content-type", contentType}});
    const auto ids = xmlValues(response.body, "UploadId");
    if (response.status != 200 || ids.empty())
        throw errorOf("starting the multipart upload of " + key, response);
    return ids.front();
}

std::string S3Client::uploadPart(const std::string& key, const std::string& uploadId, int number,
                                 const std::string& body)
{
    const HttpResponse response =
        send("PUT", key, {{"partNumber", std::to_string(number)}, {"uploadId", uploadId}}, body);
    const auto etag = response.headers.find("etag");
    if (response.status != 200 || etag == response.headers.end())
        throw errorOf("uploading part " + std::to_string(number) + " of " + key, response);
    return etag->second;
}

std::optional<std::vector<S3Part>> S3Client::listParts(const std::string& key, const std::string& uploadId)
{
    std::vector<S3Part> parts;
    std::string marker;
    for (;;) {
        std::map<std::string, std::string> query{{"uploadId", uploadId}};
        if (!marker.empty())
            query["part-number-marker"] = marker;
        const HttpResponse response = send("GET", key, query, {});
        if (response.status == 404)
            return std::nullopt;
        if (response.status != 200)
            throw errorOf("listing the uploaded parts of " + key, response);
        const auto numbers = xmlValues(response.body, "PartNumber");
        const auto etags = xmlValues(response.body, "ETag");
        const auto sizes = xmlValues(response.body, "Size");
        for (size_t i = 0; i < numbers.size() && i < etags.size() && i < sizes.size(); ++i)
            parts.push_back({std::stoi(numbers[i]), etags[i], std::stoull(sizes[i])});
        const auto truncated = xmlValues(response.body, "IsTruncated");
        const auto next = xmlValues(response.body, "NextPartNumberMarker");
        if (truncated.empty() || truncated.front() != "true" || next.empty())
            return parts;
        marker = next.front();
    }
}

void S3Client::completeMultipart(const std::string& key, const std::string& uploadId,
                                 const std::vector<S3Part>& parts)
{
    std::string body = "<CompleteMultipartUpload>";
    for (const S3Part& part : parts)
        body += "<Part><PartNumber>" + std::to_string(part.number) + "</PartNumber><ETag>" + escapeXml(part.etag) +
                "</ETag></Part>";
    body += "</CompleteMultipartUpload>";
    const HttpResponse response = send("POST", key, {{"uploadId", uploadId}}, body, {{"content-type", "application/xml"}});
    // S3 can answer 200 and still report an error in the body.
    if (response.status != 200 || response.body.find("<Error>") != std::string::npos)
        throw errorOf("completing the multipart upload of " + key, response);
}

} // namespace azl
