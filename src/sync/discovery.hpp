#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace azl {

inline constexpr const char* kServiceType = "_nocturne._tcp";

// An engine found on the network: its Bonjour name, where its protocol listens, and its TXT record ("role": "main"
// or "node").
struct Service {
    std::string name;
    std::string host;
    uint16_t port = 0;
    std::map<std::string, std::string> txt;

    bool operator==(const Service&) const = default;
};

// Announces this engine's protocol port as an _nocturne._tcp service until destroyed.
class Advertiser {
public:
    Advertiser(const std::string& name, uint16_t port, std::map<std::string, std::string> txt);
    ~Advertiser();

    void update(std::map<std::string, std::string> txt);

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

// Keeps the list of _nocturne._tcp services on the network, resolved to host and port.
class Browser {
public:
    // Called on an internal queue whenever the list changes.
    using OnChange = std::function<void(const std::vector<Service>&)>;

    explicit Browser(OnChange onChange = {});
    ~Browser();

    std::vector<Service> services() const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

// This machine's name, for advertising: the computer name, else the host name.
std::string machineName();

} // namespace azl
