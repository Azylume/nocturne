// nocturne.exe as a process on Windows: the engine rendering offscreen, and safe mode end to end.
#include "net/websocket.hpp"
#include "platform/process.hpp"
#include "protocol/messages.hpp"

#include <doctest.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using azl::Message;
using Clock = std::chrono::steady_clock;

// A nocturne.exe started with `args`, its output collected as it comes.
class Engine {
public:
    explicit Engine(std::vector<std::string> args)
    {
        const std::optional<azl::PipeEnds> pipe = azl::makePipe();
        REQUIRE(pipe);
        azl::SpawnOptions options{.program = NOCTURNE_ENGINE, .args = std::move(args)};
        options.output = azl::Stdio::to(pipe->write);
        options.error = azl::Stdio::to(pipe->write);
        std::string error;
        std::optional<azl::Process> started = azl::spawnProcess(options, &error);
        azl::closeHandle(pipe->write);
        REQUIRE_MESSAGE(started, error);
        process_ = std::move(*started);
        reader_ = std::thread([this, read = pipe->read] {
            char buffer[4096];
            for (long n; (n = azl::readHandle(read, buffer, sizeof(buffer))) > 0;) {
                std::lock_guard lock(mutex_);
                output_.append(buffer, size_t(n));
            }
            azl::closeHandle(read);
        });
    }
    ~Engine()
    {
        if (!process_.poll())
            process_.kill();
        process_.wait();
        reader_.join();
    }

    std::string output()
    {
        std::lock_guard lock(mutex_);
        return output_;
    }
    // The exit code, or nothing when it is still running after `seconds`.
    std::optional<int> exited(double seconds)
    {
        const auto until = Clock::now() + std::chrono::duration<double>(seconds);
        while (Clock::now() < until)
            if (std::optional<int> code = process_.poll())
                return code;
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return process_.poll();
    }
    azl::Process& process() { return process_; }

private:
    azl::Process process_;
    std::thread reader_;
    std::mutex mutex_;
    std::string output_;
};

// A protocol client of the engine, connecting while it starts.
class Remote {
public:
    explicit Remote(uint16_t port)
    {
        const auto until = Clock::now() + std::chrono::seconds(15);
        while (!client_ && Clock::now() < until) {
            try {
                client_ = std::make_unique<azl::WebSocketClient>("127.0.0.1", port, azl::Encoding::Json,
                                                                  [this](const Message& m) { arrived(m); });
            } catch (const std::exception&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        REQUIRE_MESSAGE(client_, "cannot connect to the engine");
    }

    Message request(const Message& message, const std::string& type)
    {
        client_->send(message);
        std::unique_lock lock(mutex_);
        for (;;) {
            REQUIRE(arrival_.wait_for(lock, std::chrono::seconds(10), [&] { return !queue_.empty(); }));
            Message next = std::move(queue_.front());
            queue_.pop_front();
            if (next["type"] == type || next["type"] == "error")
                return next;
        }
    }

private:
    void arrived(const Message& message)
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(message);
        arrival_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable arrival_;
    std::deque<Message> queue_;
    std::unique_ptr<azl::WebSocketClient> client_;
};

fs::path freshFolder(const std::string& name)
{
    const fs::path folder = fs::temp_directory_path() / ("nocturne-" + name + "-" + std::to_string(std::rand()));
    fs::remove_all(folder);
    fs::create_directories(folder);
    return folder;
}

} // namespace

TEST_CASE("windows engine: nocturne.exe renders offscreen on Vulkan, reports its rate, and stops when asked")
{
    const fs::path state = freshFolder("render");
    const uint16_t port = 7494;
    Engine engine({"--offscreen", "--port", std::to_string(port), "--loopback", "--state", state.string()});
    {
        Remote remote(port);
        CHECK(remote.request(azl::msg::hello(), "snapshot")["type"] == "snapshot");
        std::this_thread::sleep_for(std::chrono::seconds(3));
        const Message status = remote.request({{"type", "machine"}, {"op", "status"}}, "machine");
        CHECK(status["mode"] == "normal");
        INFO(status["render"].dump());
        CHECK(status["render"]["fps"].get<double>() > 50.0);
    }
    engine.process().terminate();
    CHECK(engine.exited(10.0) == 0);
    CHECK(engine.output().find("stopped") != std::string::npos);
    fs::remove_all(state);
}

TEST_CASE("windows engine: safe mode serves the machine status and the reason, and stops when asked")
{
    const fs::path state = freshFolder("safe");
    const uint16_t port = 7493;
    Engine engine({"--safe-mode", "3 crashes within a minute", "--offscreen", "--port", std::to_string(port),
                   "--state", state.string()});
    {
        Remote remote(port);
        CHECK(remote.request(azl::msg::hello(), "snapshot")["type"] == "snapshot");
        const Message status = remote.request({{"type", "machine"}, {"op", "status"}}, "machine");
        CHECK(status["mode"] == "safe");
        CHECK(status["reason"] == "3 crashes within a minute");
        CHECK(remote.request({{"type", "machine"}, {"op", "safeMode"}}, "machine")["type"] == "error");
    }
    engine.process().terminate();
    CHECK(engine.exited(5.0) == 0);
    CHECK(engine.output().find("safe mode: 3 crashes within a minute") != std::string::npos);
    CHECK(engine.output().find("stopped") != std::string::npos);
    fs::remove_all(state);
}
