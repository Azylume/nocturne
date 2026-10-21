#pragma once

#include "harness/engine_harness.hpp"
#include "protocol/messages.hpp"

#include <doctest.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

// Protocol and media helpers shared by the harness tests.
namespace {

namespace fs = std::filesystem;
namespace msg = azl::msg;
using azl::Message;

fs::path mediaDir()
{
    const char* dir = std::getenv("NOCTURNE_MEDIA_DIR");
    return dir ? dir : NOCTURNE_MEDIA_DIR;
}

class TempDir {
public:
    TempDir()
    {
        std::string pattern = (fs::temp_directory_path() / "nocturne-test-XXXXXX").string();
        REQUIRE(mkdtemp(pattern.data()));
        path = pattern;
    }
    ~TempDir() { fs::remove_all(path); }

    fs::path path;
};

[[maybe_unused]] fs::path copyMedia(const std::string& name, const fs::path& to)
{
    fs::create_directories(to.parent_path());
    fs::copy_file(mediaDir() / name, to, fs::copy_options::overwrite_existing);
    return to;
}

// A protocol client in the test. Library replies arrive from another thread, so it waits for them.
class Client {
public:
    // `loopback` false stands in for a client on another machine.
    explicit Client(azl::DocumentServer& server, bool loopback = true)
        : server_(server), id_(server.connect([this](const Message& m) { push(m); }, loopback))
    {
        send(msg::hello());
        snapshot = next();
    }
    ~Client() { server_.disconnect(id_); }

    void send(const Message& message) { server_.receive(id_, message); }

    Message next()
    {
        std::unique_lock lock(mutex_);
        REQUIRE_MESSAGE(arrived_.wait_for(lock, std::chrono::seconds(30), [&] { return messages_.size() > taken_; }),
                        "no reply");
        return messages_[taken_++];
    }

    Message snapshot;

private:
    void push(const Message& message)
    {
        std::lock_guard lock(mutex_);
        messages_.push_back(message);
        arrived_.notify_all();
    }

    azl::DocumentServer& server_;
    azl::ClientId id_;
    std::mutex mutex_;
    std::condition_variable arrived_;
    std::vector<Message> messages_;
    size_t taken_ = 0;
};

[[maybe_unused]] Message command(Client& client, const azl::Edit& edit)
{
    client.send(msg::command(edit));
    return client.next();
}

// Returns the change the edit caused.
[[maybe_unused]] Message apply(Client& client, const azl::Edit& edit)
{
    const Message reply = command(client, edit);
    REQUIRE_MESSAGE(reply["type"] == "change", reply.dump());
    return reply;
}

Message addMedia(Client& client, const fs::path& path)
{
    client.send({{"type", "library"}, {"op", "add"}, {"path", path.string()}});
    return client.next();
}

Message setClip(Client& client, const std::string& hash, const std::string& name = "clip",
                azl::Transport transport = {})
{
    client.send(msg::command(azl::SetLayerClip{1, azl::ClipRef{hash, name, transport}}));
    return client.next();
}

[[maybe_unused]] Message playMedia(Client& client, const fs::path& path, azl::Transport transport = {})
{
    const Message added = addMedia(client, path);
    REQUIRE(added["type"] == "library");
    const Message& entry = added["entry"];
    REQUIRE(entry["status"] == "ok");
    REQUIRE(setClip(client, entry["hash"], entry["name"], transport)["type"] == "change");
    return entry;
}

[[maybe_unused]] const uint8_t* pixel(const azl::Image& image, uint32_t x, uint32_t y)
{
    return &image.rgba[(size_t(y) * image.width + x) * 4];
}

} // namespace
