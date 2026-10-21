#include "upload/media_fetch.hpp"

#include "media/media_library.hpp"

#include <algorithm>
#include <fstream>

namespace azl {

namespace fs = std::filesystem;

namespace {

bool holds(const fs::path& file, const std::string& hash)
{
    std::error_code ec;
    if (!fs::is_regular_file(file, ec))
        return false;
    try {
        return sha256File(file) == hash;
    } catch (const std::runtime_error&) {
        return false;
    }
}

} // namespace

fs::path fetchedFile(const fs::path& folder, const std::string& hash)
{
    return folder / (hash + ".mp4");
}

void fetchObject(S3Client& client, const std::string& hash, const fs::path& file, uint64_t chunk)
{
    if (holds(file, hash))
        return;
    const std::optional<uint64_t> size = client.head(hash);
    if (!size)
        throw std::runtime_error("media " + hash + " is not in the bucket");
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path part = file.string() + ".part";
    uint64_t have = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;
    if (ec || have > *size) {
        fs::remove(part, ec);
        have = 0;
    }
    std::ofstream out(part, std::ios::binary | std::ios::app);
    while (have < *size) {
        const std::optional<std::string> data = client.getRange(hash, have, std::min(chunk, *size - have));
        if (!data)
            throw std::runtime_error("media " + hash + " left the bucket");
        if (data->empty())
            throw std::runtime_error("media " + hash + ": the bucket sent nothing at " + std::to_string(have));
        out.write(data->data(), std::streamsize(data->size()));
        out.flush();
        if (!out)
            throw std::runtime_error("cannot write " + part.string());
        have += data->size();
    }
    out.close();
    if (!holds(part, hash)) {
        fs::remove(part, ec);
        throw std::runtime_error("media " + hash + " arrived with other content; deleted");
    }
    fs::rename(part, file, ec);
    if (ec)
        throw std::runtime_error("cannot move " + part.string() + " into place: " + ec.message());
}

MediaFetcher::MediaFetcher(ClientSource client, fs::path folder, Arrived arrived, Changed changed)
    : source_(std::move(client)), folder_(std::move(folder)), arrived_(std::move(arrived)),
      changed_(std::move(changed)), thread_([this] { run(); })
{
}

MediaFetcher::~MediaFetcher()
{
    {
        std::lock_guard lock(mutex_);
        running_ = false;
    }
    wake_.notify_all();
    thread_.join();
}

void MediaFetcher::want(const std::string& hash)
{
    {
        std::lock_guard lock(mutex_);
        if (fetching_ == hash || std::ranges::find(queue_, hash) != queue_.end())
            return;
        failed_.erase(hash);
        queue_.push_back(hash);
    }
    wake_.notify_all();
}

void MediaFetcher::cancel(const std::string& hash)
{
    std::lock_guard lock(mutex_);
    std::erase(queue_, hash);
    failed_.erase(hash);
}

std::optional<MediaFetcher::State> MediaFetcher::state(const std::string& hash) const
{
    std::lock_guard lock(mutex_);
    if (fetching_ == hash)
        return State::Fetching;
    if (std::ranges::find(queue_, hash) != queue_.end())
        return State::Queued;
    if (failed_.contains(hash))
        return State::Failed;
    return std::nullopt;
}

std::string MediaFetcher::error(const std::string& hash) const
{
    std::lock_guard lock(mutex_);
    auto it = failed_.find(hash);
    return it == failed_.end() ? std::string() : it->second;
}

void MediaFetcher::run()
{
    std::unique_lock lock(mutex_);
    while (true) {
        wake_.wait(lock, [this] { return !running_ || !queue_.empty(); });
        if (!running_)
            return;
        fetching_ = queue_.front();
        queue_.pop_front();
        const std::string hash = *fetching_;
        lock.unlock();
        fetch(hash);
        lock.lock();
    }
}

void MediaFetcher::fetch(const std::string& hash)
{
    if (changed_)
        changed_(hash);
    std::string error;
    const fs::path file = fetchedFile(folder_, hash);
    try {
        if (!client_)
            client_ = source_();
        fetchObject(*client_, hash, file);
    } catch (const std::exception& e) {
        error = e.what();
    }
    if (error.empty() && arrived_)
        arrived_(hash, file);
    {
        std::lock_guard lock(mutex_);
        fetching_.reset();
        if (!error.empty())
            failed_[hash] = error;
    }
    if (changed_)
        changed_(hash);
}

} // namespace azl
