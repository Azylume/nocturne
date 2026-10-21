#pragma once

#include "media/keep_local.hpp"
#include "media/media_library.hpp"
#include "upload/media_fetch.hpp"
#include "upload/upload_states.hpp"

#include <dispatch/dispatch.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace azl::ui {

// The Director's own media: its Media Library, which media it keeps (KeepLocal), stills of every clip cached by hash,
// and fetches of chosen media from the bucket. Media it lacks stays in the bucket and on the venues; removing a local
// copy never touches either. Call it from one thread, except has() and still(), which any thread may call.
class DirectorMedia {
public:
    struct Options {
        // This machine's library; empty for none (nothing is local).
        std::filesystem::path library;
        // Empty: the choices are kept in memory only.
        std::filesystem::path keepLocal;
        // Empty: stills are not kept on disk.
        std::filesystem::path stills;
        // Where fetched media lands; files here not chosen any more are deleted.
        std::filesystem::path fetched;
        // The bucket; null for no fetching.
        MediaFetcher::ClientSource bucket;
        // This machine's uploads (UploadStates); empty for none.
        std::filesystem::path uploads;
    };

    // The interactive Director's: the default library, choices, stills and fetch folder, and the R2 settings.
    static Options defaults();

    // `changed` runs on any thread with a hash whose media arrived or left, whose fetch changed, or whose still was
    // rendered here.
    DirectorMedia(Options options, std::function<void(const std::string& hash)> changed);
    ~DirectorMedia();

    // Null without a library.
    MediaLibrary* library() { return library_.get(); }
    // This machine's library can play the media; asked of the library once, then kept current by what arrives and
    // leaves.
    bool has(const std::string& hash);

    // The clip's still: from the cache, or rendered from local media on a background queue (empty until it is ready).
    // Nothing when the venue must send it.
    std::optional<std::vector<uint8_t>> still(const std::string& hash);
    // A still the venue sent.
    void storeStill(const std::string& hash, const std::vector<uint8_t>& jpeg);

    const KeepLocal& keep() const { return keep_; }
    void setClip(const std::string& hash, std::optional<bool> keep);
    void setShow(const std::string& show, bool keep, const std::set<std::string>& media);
    // The choices file changed on disk (ctl): true when it did; reconcile then.
    bool reload() { return keep_.reload(); }
    // The uploads file changed on disk: true when it did.
    bool reloadUploads() { return uploads_.reload(); }

    // Brings this machine to the choices: fetches chosen media it lacks, deletes fetched media no longer chosen and
    // forgets media chosen not to keep. `show` (empty for none) is the venue's show now, using `media`.
    void reconcile(const std::string& show, const std::set<std::string>& media);

    enum class Where { Local, Queued, Fetching, Failed, Remote };
    Where where(const std::string& hash);
    // Why the last fetch of the media failed.
    std::string fetchError(const std::string& hash) const;

    // Unknown: never uploaded from here and not on this machine either.
    enum class Upload { Unknown, LocalOnly, Uploading, Uploaded, Failed };
    Upload upload(const std::string& hash);
    std::optional<UploadState> uploadState(const std::string& hash) const { return uploads_.find(hash); }

private:
    void arrived(const std::string& hash, const std::filesystem::path& file);
    void setPresent(const std::string& hash, bool present);
    void remove(const std::string& hash, bool deleteFile);
    void renderStill(const std::string& hash);

    Options options_;
    std::function<void(const std::string&)> changed_;
    std::unique_ptr<MediaLibrary> library_;
    KeepLocal keep_;
    UploadStates uploads_;
    StillCache stills_;
    std::mutex mutex_;
    std::map<std::string, bool> present_;
    std::set<std::string> rendering_;
    std::map<std::string, std::vector<uint8_t>> rendered_;
    dispatch_queue_t renders_;
    std::unique_ptr<MediaFetcher> fetcher_;
};

} // namespace azl::ui
