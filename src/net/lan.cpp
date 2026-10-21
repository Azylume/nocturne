#include "net/lan.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>

namespace azl {

namespace {

std::optional<uint32_t> ipv4(const std::string& address)
{
    in_addr parsed{};
    if (inet_pton(AF_INET, address.c_str(), &parsed) != 1)
        return std::nullopt;
    return ntohl(parsed.s_addr);
}

bool inRange(uint32_t address, uint32_t base, int bits)
{
    const uint32_t mask = bits == 0 ? 0 : ~uint32_t(0) << (32 - bits);
    return (address & mask) == (base & mask);
}

std::optional<std::array<uint8_t, 16>> ipv6(const std::string& address)
{
    std::string bare = address.substr(0, address.find('%'));
    in6_addr parsed{};
    if (inet_pton(AF_INET6, bare.c_str(), &parsed) != 1)
        return std::nullopt;
    std::array<uint8_t, 16> bytes{};
    std::memcpy(bytes.data(), &parsed, 16);
    return bytes;
}

// An IPv4-mapped IPv6 address as its IPv4 address.
std::optional<uint32_t> mapped(const std::array<uint8_t, 16>& b)
{
    for (int i = 0; i < 10; ++i)
        if (b[i] != 0)
            return std::nullopt;
    if (b[10] != 0xff || b[11] != 0xff)
        return std::nullopt;
    return uint32_t(b[12]) << 24 | uint32_t(b[13]) << 16 | uint32_t(b[14]) << 8 | b[15];
}

bool privateIpv4(uint32_t a)
{
    return inRange(a, 0x0a000000, 8) || inRange(a, 0xac100000, 12) || inRange(a, 0xc0a80000, 16) ||
           inRange(a, 0xa9fe0000, 16);
}

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

std::string withoutPort(const std::string& host)
{
    if (!host.empty() && host.front() == '[')
        return host.substr(1, host.find(']') - 1);
    const size_t colon = host.find(':');
    return colon != std::string::npos && host.find(':', colon + 1) == std::string::npos ? host.substr(0, colon) : host;
}

} // namespace

bool privateAddress(const std::string& address)
{
    if (const auto a = ipv4(address))
        return privateIpv4(*a);
    const auto b = ipv6(address);
    if (!b)
        return false;
    if (const auto a = mapped(*b))
        return privateIpv4(*a);
    return ((*b)[0] == 0xfe && ((*b)[1] & 0xc0) == 0x80) || ((*b)[0] & 0xfe) == 0xfc;
}

bool loopbackAddress(const std::string& address)
{
    if (const auto a = ipv4(address))
        return inRange(*a, 0x7f000000, 8);
    const auto b = ipv6(address);
    if (!b)
        return false;
    if (const auto a = mapped(*b))
        return inRange(*a, 0x7f000000, 8);
    std::array<uint8_t, 16> one{};
    one[15] = 1;
    return *b == one;
}

std::vector<std::string> lanAddresses()
{
    std::vector<std::string> found;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0)
        return found;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) ||
            (i->ifa_flags & IFF_LOOPBACK) || std::strncmp(i->ifa_name, "utun", 4) == 0)
            continue;
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, text, sizeof text);
        const std::string address = text;
        if (privateAddress(address) && std::find(found.begin(), found.end(), address) == found.end())
            found.push_back(address);
    }
    freeifaddrs(list);
    return found;
}

std::string bonjourName()
{
    char name[256] = {};
    gethostname(name, sizeof name - 1);
    std::string host = name;
    const size_t dot = host.find('.');
    return (dot == std::string::npos ? host : host.substr(0, dot)) + ".local";
}

std::optional<std::string> deskRefusal(const std::string& source, const std::string& host,
                                       const std::vector<std::string>& own, const std::string& name, bool loopback)
{
    if (!privateAddress(source) && !(loopback && loopbackAddress(source)))
        return "the desk answers only on the venue's own network, not " + source;
    const std::string asked = lower(withoutPort(host));
    if (asked == lower(name) || std::find(own.begin(), own.end(), asked) != own.end())
        return std::nullopt;
    if (loopback && (asked == "localhost" || asked == "127.0.0.1"))
        return std::nullopt;
    return "the desk does not answer to the name " + (host.empty() ? std::string("(none)") : host);
}

} // namespace azl
