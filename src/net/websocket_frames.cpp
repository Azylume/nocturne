#include "net/websocket_frames.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace azl::ws {

namespace {

constexpr const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr size_t kMaxControlPayload = 125;

uint32_t rotl(uint32_t value, int bits)
{
    return value << bits | value >> (32 - bits);
}

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

std::string trimmed(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t");
    const size_t last = text.find_last_not_of(" \t");
    return first == std::string::npos ? "" : text.substr(first, last - first + 1);
}

// A comma-separated header holds `token`, in any case.
bool listsToken(const Head& head, const std::string& name, const std::string& token)
{
    const auto found = head.headers.find(name);
    if (found == head.headers.end())
        return false;
    const std::string value = lower(found->second);
    for (size_t at = 0; at <= value.size();) {
        const size_t comma = std::min(value.find(',', at), value.size());
        if (trimmed(value.substr(at, comma - at)) == token)
            return true;
        at = comma + 1;
    }
    return false;
}

std::string header(const Head& head, const std::string& name)
{
    const auto found = head.headers.find(name);
    return found == head.headers.end() ? "" : found->second;
}

std::string refusal(const std::string& status)
{
    return "HTTP/1.1 " + status + "\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
}

bool controlOpcode(Opcode op)
{
    return (uint8_t(op) & 0x8) != 0;
}

bool knownOpcode(uint8_t op)
{
    return op <= 0x2 || (op >= 0x8 && op <= 0xa);
}

} // namespace

std::array<uint8_t, 20> sha1(const void* data, size_t size)
{
    uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    std::vector<uint8_t> message(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    message.push_back(0x80);
    while (message.size() % 64 != 56)
        message.push_back(0);
    const uint64_t bits = uint64_t(size) * 8;
    for (int shift = 56; shift >= 0; shift -= 8)
        message.push_back(uint8_t(bits >> shift));
    for (size_t chunk = 0; chunk < message.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const uint8_t* b = &message[chunk + size_t(i) * 4];
            w[i] = uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5a827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdc;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6;
            }
            const uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::array<uint8_t, 20> digest{};
    for (int i = 0; i < 20; ++i)
        digest[size_t(i)] = uint8_t(h[i / 4] >> (24 - 8 * (i % 4)));
    return digest;
}

std::string base64(const uint8_t* data, size_t size)
{
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t n = uint32_t(data[i]) << 16 | (i + 1 < size ? uint32_t(data[i + 1]) << 8 : 0) |
                           (i + 2 < size ? uint32_t(data[i + 2]) : 0);
        out += kAlphabet[n >> 18 & 63];
        out += kAlphabet[n >> 12 & 63];
        out += i + 1 < size ? kAlphabet[n >> 6 & 63] : '=';
        out += i + 2 < size ? kAlphabet[n & 63] : '=';
    }
    return out;
}

std::string acceptKey(const std::string& key)
{
    const std::string text = key + kGuid;
    const std::array<uint8_t, 20> digest = sha1(text.data(), text.size());
    return base64(digest.data(), digest.size());
}

size_t headEnd(const std::string& text)
{
    const size_t end = text.find("\r\n\r\n");
    return end == std::string::npos ? end : end + 4;
}

std::optional<Head> parseHead(const std::string& text)
{
    const size_t end = headEnd(text);
    if (end == std::string::npos)
        return std::nullopt;
    Head head;
    size_t at = 0;
    for (bool first = true; at + 2 < end; first = false) {
        const size_t next = text.find("\r\n", at);
        const std::string line = text.substr(at, next - at);
        at = next + 2;
        if (first) {
            head.start = line;
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0)
            return std::nullopt;
        std::string& value = head.headers[lower(line.substr(0, colon))];
        value += (value.empty() ? "" : ", ") + trimmed(line.substr(colon + 1));
    }
    return head;
}

HandshakeAnswer answerHandshake(const Head& request)
{
    if (request.headers.contains("origin"))
        return {false, refusal("403 Forbidden")};
    const std::string key = header(request, "sec-websocket-key");
    if (!request.start.starts_with("GET ") || !request.start.ends_with(" HTTP/1.1") ||
        !listsToken(request, "upgrade", "websocket") || !listsToken(request, "connection", "upgrade") || key.empty())
        return {false, refusal("400 Bad Request")};
    if (header(request, "sec-websocket-version") != "13")
        return {false, "HTTP/1.1 426 Upgrade Required\r\nSec-WebSocket-Version: 13\r\nConnection: close\r\n"
                       "Content-Length: 0\r\n\r\n"};
    return {true, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Accept: " + acceptKey(key) + "\r\n\r\n"};
}

std::string handshakeRequest(const std::string& hostHeader, const std::string& key)
{
    return "GET / HTTP/1.1\r\nHost: " + hostHeader + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
}

bool handshakeAccepted(const Head& response, const std::string& key)
{
    return response.start.starts_with("HTTP/1.1 101") && listsToken(response, "upgrade", "websocket") &&
           listsToken(response, "connection", "upgrade") && header(response, "sec-websocket-accept") == acceptKey(key);
}

std::vector<uint8_t> frame(Opcode opcode, const uint8_t* payload, size_t size,
                           const std::optional<std::array<uint8_t, 4>>& mask)
{
    std::vector<uint8_t> out;
    out.reserve(size + 14);
    out.push_back(uint8_t(0x80 | uint8_t(opcode)));
    const uint8_t masked = mask ? 0x80 : 0;
    if (size < 126) {
        out.push_back(uint8_t(masked | size));
    } else if (size <= 0xffff) {
        out.push_back(masked | 126);
        out.push_back(uint8_t(size >> 8));
        out.push_back(uint8_t(size));
    } else {
        out.push_back(masked | 127);
        for (int shift = 56; shift >= 0; shift -= 8)
            out.push_back(uint8_t(uint64_t(size) >> shift));
    }
    if (!mask) {
        out.insert(out.end(), payload, payload + size);
        return out;
    }
    out.insert(out.end(), mask->begin(), mask->end());
    for (size_t i = 0; i < size; ++i)
        out.push_back(payload[i] ^ (*mask)[i % 4]);
    return out;
}

std::vector<uint8_t> closeFrame(uint16_t code, const std::optional<std::array<uint8_t, 4>>& mask)
{
    const uint8_t payload[2] = {uint8_t(code >> 8), uint8_t(code)};
    return frame(Opcode::Close, payload, sizeof payload, mask);
}

void MessageReader::feed(const uint8_t* data, size_t size)
{
    if (failed_)
        return;
    if (offset_ > 0 && offset_ >= buffer_.size() / 2) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + std::ptrdiff_t(offset_));
        offset_ = 0;
    }
    buffer_.insert(buffer_.end(), data, data + size);
}

std::optional<Event> MessageReader::fail(Event::Kind kind)
{
    failed_ = true;
    buffer_.clear();
    message_.clear();
    offset_ = 0;
    return Event{kind, {}};
}

std::optional<Event> MessageReader::next()
{
    while (!failed_) {
        const uint8_t* at = buffer_.data() + offset_;
        const size_t available = buffer_.size() - offset_;
        if (available < 2)
            return std::nullopt;
        const bool fin = at[0] & 0x80;
        const uint8_t rawOpcode = at[0] & 0x0f;
        const bool masked = at[1] & 0x80;
        if ((at[0] & 0x70) != 0 || !knownOpcode(rawOpcode) || masked != fromClient_)
            return fail(Event::Kind::Invalid);
        const Opcode opcode = Opcode(rawOpcode);
        const bool control = controlOpcode(opcode);
        uint64_t size = at[1] & 0x7f;
        size_t header = 2;
        if (size >= 126) {
            const size_t extended = size == 126 ? 2 : 8;
            if (available < header + extended)
                return std::nullopt;
            size = 0;
            for (size_t i = 0; i < extended; ++i)
                size = size << 8 | at[header + i];
            header += extended;
            if (size >> 63)
                return fail(Event::Kind::Invalid);
        }
        if (control && (!fin || size > kMaxControlPayload))
            return fail(Event::Kind::Invalid);
        if (!control) {
            const bool continuation = opcode == Opcode::Continuation;
            if (continuation != fragmented_.has_value())
                return fail(Event::Kind::Invalid);
            if (size > limit_ || message_.size() > limit_ - size)
                return fail(Event::Kind::TooBig);
        }
        const size_t maskBytes = masked ? 4 : 0;
        if (available - header < maskBytes || available - header - maskBytes < size)
            return std::nullopt;
        const uint8_t* key = at + header;
        const uint8_t* payload = key + maskBytes;
        std::vector<uint8_t> unmasked(payload, payload + size);
        if (masked)
            for (size_t i = 0; i < unmasked.size(); ++i)
                unmasked[i] ^= key[i % 4];
        offset_ += header + maskBytes + size_t(size);

        if (control) {
            const Event::Kind kind = opcode == Opcode::Ping   ? Event::Kind::Ping
                                     : opcode == Opcode::Pong ? Event::Kind::Pong
                                                              : Event::Kind::Close;
            if (kind == Event::Kind::Close && unmasked.size() == 1)
                return fail(Event::Kind::Invalid);
            return Event{kind, std::move(unmasked)};
        }
        if (!fragmented_)
            fragmented_ = opcode;
        message_.insert(message_.end(), unmasked.begin(), unmasked.end());
        if (!fin)
            continue;
        const Event::Kind kind = *fragmented_ == Opcode::Text ? Event::Kind::Text : Event::Kind::Binary;
        fragmented_.reset();
        return Event{kind, std::exchange(message_, {})};
    }
    return std::nullopt;
}

} // namespace azl::ws
