#include "ui/client/connection.hpp"

#include "net/websocket.hpp"

namespace azl::ui {

Connection::Connection(EngineClient& client, std::string host, uint16_t port, bool surfaces, std::string token)
    : client_(client), host_(std::move(host)), port_(port), surfaces_(surfaces), token_(std::move(token)),
      thread_([this] { run(); })
{
}

Connection::~Connection()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

std::string Connection::lastError() const
{
    std::lock_guard lock(mutex_);
    return lastError_;
}

void Connection::run()
{
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        lock.unlock();
        std::shared_ptr<WebSocketClient> socket;
        std::string error;
        try {
            socket = std::make_shared<WebSocketClient>(
                host_, port_, Encoding::MessagePack, [this](const Message& message) { client_.receive(message); },
                [this] {
                    std::lock_guard guard(mutex_);
                    closed_ = true;
                    wake_.notify_all();
                });
        } catch (const std::exception& e) {
            error = e.what();
        }
        lock.lock();
        lastError_ = error;
        if (socket && !stopping_) {
            closed_ = false;
            lock.unlock();
            std::weak_ptr<WebSocketClient> weak = socket;
            client_.attached(
                [weak](const Message& message) {
                    if (auto open = weak.lock())
                        open->send(message);
                },
                surfaces_ && isLocalHost(host_), token_);
            lock.lock();
            wake_.wait(lock, [&] { return stopping_ || closed_; });
            lock.unlock();
            client_.detached();
            socket.reset();
            lock.lock();
        } else if (socket) {
            lock.unlock();
            socket.reset();
            lock.lock();
        }
        wake_.wait_for(lock, std::chrono::seconds(1), [&] { return stopping_; });
    }
}

} // namespace azl::ui
