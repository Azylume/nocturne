#pragma once

#include <optional>
#include <string>
#include <vector>

namespace azl {

// This machine's IPv4 addresses on its LAN interfaces: up, not loopback, not a tunnel (utun, Tailscale's
// 100.64.0.0/10), private or link-local; in interface order.
std::vector<std::string> lanAddresses();
// Every numeric address, IPv4 and IPv6, of every interface of this machine.
std::vector<std::string> interfaceAddresses();
// The machine's Bonjour name, "<host>.local".
std::string bonjourName();

// IPv4 10/8, 172.16/12, 192.168/16, and link-local 169.254/16; IPv6 link-local fe80::/10 and unique local fc00::/7.
bool privateAddress(const std::string& address);
bool loopbackAddress(const std::string& address);

// Why a request to the venue desk is refused: its source is not on the LAN (private or link-local; loopback only
// when `loopback`), or its Host header (port aside, any case) is neither one of `own` addresses nor `name`
// ("<host>.local"; with `loopback` also localhost and 127.0.0.1). None when it may go through.
std::optional<std::string> deskRefusal(const std::string& source, const std::string& host,
                                       const std::vector<std::string>& own, const std::string& name, bool loopback);

} // namespace azl
