#include "net/lan.hpp"

#include "net/socket_compat.hpp"

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif

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

#ifdef _WIN32

namespace {

// Every adapter with its addresses, or none when the call fails.
std::vector<uint8_t> adapters()
{
    ULONG size = 16 << 10;
    std::vector<uint8_t> buffer;
    for (int attempt = 0; attempt < 3; ++attempt) {
        buffer.resize(size);
        const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        const ULONG result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
        if (result == NO_ERROR)
            return buffer;
        if (result != ERROR_BUFFER_OVERFLOW)
            break;
    }
    return {};
}

std::string numeric(const SOCKET_ADDRESS& address)
{
    char text[NI_MAXHOST] = {};
    if (getnameinfo(address.lpSockaddr, address.iSockaddrLength, text, sizeof text, nullptr, 0, NI_NUMERICHOST) != 0)
        return {};
    return text;
}

// Calls `use` with each unicast address of every adapter that is up.
template <typename Use> void eachAddress(Use use)
{
    startSockets();
    std::vector<uint8_t> buffer = adapters();
    if (buffer.empty())
        return;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp)
            continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
            use(*a, u->Address);
    }
}

} // namespace

std::vector<std::string> lanAddresses()
{
    std::vector<std::string> found;
    eachAddress([&](const IP_ADAPTER_ADDRESSES& adapter, const SOCKET_ADDRESS& at) {
        if (at.lpSockaddr->sa_family != AF_INET || adapter.IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
            adapter.IfType == IF_TYPE_TUNNEL)
            return;
        const std::string address = numeric(at);
        if (privateAddress(address) && std::find(found.begin(), found.end(), address) == found.end())
            found.push_back(address);
    });
    return found;
}

std::vector<std::string> interfaceAddresses()
{
    std::vector<std::string> found;
    eachAddress([&](const IP_ADAPTER_ADDRESSES&, const SOCKET_ADDRESS& at) {
        if (at.lpSockaddr->sa_family == AF_INET || at.lpSockaddr->sa_family == AF_INET6)
            if (std::string address = numeric(at); !address.empty())
                found.push_back(std::move(address));
    });
    return found;
}

#else

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

std::vector<std::string> interfaceAddresses()
{
    std::vector<std::string> addresses;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0)
        return addresses;
    for (const ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || (a->ifa_addr->sa_family != AF_INET && a->ifa_addr->sa_family != AF_INET6))
            continue;
        const socklen_t size = a->ifa_addr->sa_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
        char text[NI_MAXHOST];
        if (getnameinfo(a->ifa_addr, size, text, sizeof text, nullptr, 0, NI_NUMERICHOST) == 0)
            addresses.push_back(text);
    }
    freeifaddrs(list);
    return addresses;
}

#endif

std::string bonjourName()
{
    startSockets();
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
