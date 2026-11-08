#pragma once

#include "net/websocket.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace azl {

// The messages a WebSocketClient received, for the thread that waits for them.
class CtlQueue {
public:
    void push(Message m)
    {
        std::lock_guard lock(mutex_);
        messages_.push_back(std::move(m));
        changed_.notify_all();
    }

    void close()
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        changed_.notify_all();
    }

    bool closed()
    {
        std::lock_guard lock(mutex_);
        return closed_ && messages_.empty();
    }

    // Null when the connection closed or nothing came within the timeout.
    std::optional<Message> pop(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, timeout, [&] { return !messages_.empty() || closed_; }) || messages_.empty())
            return std::nullopt;
        Message m = std::move(messages_.front());
        messages_.pop_front();
        return m;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Message> messages_;
    bool closed_ = false;
};

// Says hello and waits for the snapshot; none when the engine says nothing in time. Throws with the engine's words when
// it refuses, e.g. without its pairing token.
inline std::optional<Message> greet(WebSocketClient& client, CtlQueue& queue, const std::string& token)
{
    client.send(msg::hello(token));
    std::optional<Message> snapshot = queue.pop(std::chrono::seconds(5));
    if (snapshot && (*snapshot)["type"] == "error")
        throw std::runtime_error(snapshot->value("message", "the engine refused the connection"));
    return snapshot;
}

} // namespace azl
