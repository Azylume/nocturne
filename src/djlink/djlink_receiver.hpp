#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace azl::djlink {

// Listens to the Pro DJ Link ports on one network interface, and only listens. It owns receive sockets only and has
// no way to send: no send call, no connect, no multicast membership (Pro DJ Link is broadcast), so it never announces
// itself, never claims a player number and never puts a packet on the DJ network. The DJ link test suite checks the
// module's sources for any send path.
//
// While the interface is missing it retries every kRetry seconds; when the interface comes back with a new index (a
// USB adapter plugged in again) it opens again.
class Receiver {
public:
    static constexpr double kRetry = 2.0;

    struct Options {
        std::string interface;
        // Where to bind; the default receives broadcasts. Tests bind to loopback.
        std::string address = "0.0.0.0";
        // The ports for 50000, 50001 and 50002; tests use free ones.
        std::array<uint16_t, 3> ports{50000, 50001, 50002};
    };
    // `port` is the Pro DJ Link port the datagram came to (50000..50002), whatever port it was bound on. Runs on the
    // receiver's thread.
    using OnDatagram = std::function<void(uint16_t port, const uint8_t* data, size_t size)>;

    Receiver(Options options, OnDatagram onDatagram);
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    bool listening() const { return listening_; }
    // Why it is not listening; empty while it is.
    std::string error() const;
    uint64_t datagrams() const { return datagrams_; }

private:
    void run();
    bool open();
    void close();
    void setError(std::string error);

    const Options options_;
    const OnDatagram onDatagram_;
    std::array<int, 3> sockets_{-1, -1, -1};
    unsigned index_ = 0;
    std::atomic<bool> running_{true};
    std::atomic<bool> listening_{false};
    std::atomic<uint64_t> datagrams_{0};
    mutable std::mutex errorMutex_;
    std::string error_;
    std::thread thread_;
};

} // namespace azl::djlink
