#pragma once

#include "upload/s3.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace azl {

// Brings the object `hash` down from the bucket into `file`, in ranged chunks into FILE.part, so a run cut off midway
// goes on where it stopped. Its content must hash to `hash`; a mismatch is deleted and throws. A file already there
// with that content is kept. Throws S3Error, TransportError or std::runtime_error.
void fetchObject(S3Client& client, const std::string& hash, const std::filesystem::path& file,
                 uint64_t chunk = uint64_t(4) << 20);

// Where fetchObject puts the object `hash` in `folder`.
std::filesystem::path fetchedFile(const std::filesystem::path& folder, const std::string& hash);

// The Director's fetches of chosen media: one object at a time on its own thread, from the bucket by hash into
// `folder`, each handed to `arrived` with its file (on the fetch thread). A failed fetch is kept with its error until
// it is wanted again.
class MediaFetcher {
public:
    // The bucket; throws std::runtime_error saying what to set up when it cannot be reached.
    using ClientSource = std::function<std::shared_ptr<S3Client>()>;
    using Arrived = std::function<void(const std::string& hash, const std::filesystem::path& file)>;
    // Called on the fetch thread whenever a fetch starts, ends or fails.
    using Changed = std::function<void(const std::string& hash)>;

    enum class State { Queued, Fetching, Failed };

    MediaFetcher(ClientSource client, std::filesystem::path folder, Arrived arrived, Changed changed = {});
    ~MediaFetcher();

    // Queues the hash unless it is queued or being fetched.
    void want(const std::string& hash);
    // Drops it from the queue, or forgets its failure; a fetch under way runs to its end.
    void cancel(const std::string& hash);
    // Nothing: neither queued, fetching nor failed.
    std::optional<State> state(const std::string& hash) const;
    std::string error(const std::string& hash) const;
    const std::filesystem::path& folder() const { return folder_; }

private:
    void run();
    void fetch(const std::string& hash);

    ClientSource source_;
    std::filesystem::path folder_;
    Arrived arrived_;
    Changed changed_;
    std::shared_ptr<S3Client> client_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool running_ = true;
    std::deque<std::string> queue_;
    std::optional<std::string> fetching_;
    std::map<std::string, std::string> failed_;
    std::thread thread_;
};

} // namespace azl
