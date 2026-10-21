#pragma once

#include "document/composition.hpp"
#include "document/wall_clock.hpp"
#include "media/media_library.hpp"
#include "protocol/document_server.hpp"
#include "upload/s3.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace azl {

// A media file the composition uses, and the decks whose columns hold it.
struct NeededMedia {
    std::string hash;
    std::string name;
    std::set<uint64_t> decks;

    bool operator==(const NeededMedia&) const = default;
};

// Every clip with media in every deck, by hash; text and generator clips have none.
std::vector<NeededMedia> neededMedia(const Composition& composition);

struct DeckMissing {
    uint64_t deck = 0;
    std::string name;
    int missing = 0;

    bool operator==(const DeckMissing&) const = default;
};

// Ready when nothing needed is missing; `decks` lists the decks missing something, in deck order.
struct Readiness {
    int needed = 0;
    int missing = 0;
    std::vector<DeckMissing> decks;

    bool ready() const { return missing == 0; }
};

Readiness readiness(const Composition& composition, const std::vector<NeededMedia>& needed,
                    const std::set<std::string>& present);

enum class SyncOverride { Auto, Allow, Block };

const char* overrideName(SyncOverride mode);
std::optional<SyncOverride> overrideFromName(const std::string& name);

// The one place that decides whether media may come down now: a forced allow or block, else only outside every Gig
// window (inGigWindow's "sync" channel).
bool syncAllowed(const std::vector<GigWindow>& gigs, double now, SyncOverride mode,
                 const UtcOffset& zone = systemUtcOffset);

inline constexpr double kReadinessAlertBefore = 2 * 3600.0;

// While not ready, from kReadinessAlertBefore before a Gig window starts until it ends: what to tell the operator.
std::optional<std::string> readinessAlert(const std::vector<GigWindow>& gigs, double now, const Readiness& readiness,
                                          const UtcOffset& zone = systemUtcOffset);

// The venue's read-only R2 token, as loadR2Config reads it from ~/Library/Application Support/Nocturne/r2-read.json,
// the keychain service nocturne-r2-read and the NOCTURNE_R2_READ_* variables. Throws std::runtime_error.
S3Config loadVenueR2Config();

struct MediaSyncOptions {
    // Where the override is kept; empty keeps it in memory only.
    std::filesystem::path settings;
    // Downloads land here and join the Media Library.
    std::filesystem::path folder;
    // The bucket, with a read-only token; null when none is set up, `clientError` saying why.
    std::shared_ptr<S3Client> client;
    std::string clientError;
    uint64_t chunk = 4ull << 20;
    // 0: unbounded.
    double bytesPerSecond = 2.5e6;
    // Between checks while nothing is to do.
    std::chrono::milliseconds every{30000};
    // After a failed download.
    std::chrono::milliseconds retry{30000};
    // Downloads of one object per check before a hash mismatch counts as an error.
    int attempts = 3;
    UtcOffset zone = systemUtcOffset;
};

// Brings the media the venue's own composition needs down from R2, as "mediaSync" messages say:
//
//   {"op": "status"} -> {"op": "status", "ready", "needed", "missing", "decks": [{"deck", "name", "missing"}],
//     "override": "auto" | "allow" | "block", "inWindow", "allowed", "state", "download": {"hash", "name", "bytes",
//     "size"} | null, "error": text | null, "alert": text | null}
//   {"op": "set", "override": "auto" | "allow" | "block"} -> kept, and every client gets the status
//
// "state" is "ready", "checking", "downloading", "waiting" (inside a Gig window), "blocked" (forced) or "error". A
// check runs on its own thread at background priority: the needed set is diffed against the Media Library (a file
// imported by cable counts like a downloaded one), and each missing object comes down one at a time in ranged chunks
// at a bounded rate into a .part file, so a dropped connection resumes where it stopped. It is hash-verified before
// it joins the library; a mismatch is deleted and fetched again. A download stops between chunks as soon as the
// window or the override forbids it. Every message has "type": "mediaSync"; errors are {"type": "error"}.
class MediaSync {
public:
    // Unix seconds on the venue's clock.
    using Now = std::function<double()>;
    using CompositionSource = std::function<std::shared_ptr<const Composition>()>;

    MediaSync(DocumentServer& document, MediaLibrary& library, CompositionSource composition, Now now,
              MediaSyncOptions options, std::function<void(const std::string&)> found = {});
    ~MediaSync();

    // The status message's fields without "type" and "op".
    Message status() const;
    SyncOverride mode() const;
    // Throws std::runtime_error when it cannot be kept.
    void setMode(SyncOverride mode);
    // Checks again now.
    void wake();

private:
    // Failed: this object; Offline: the bucket cannot be reached or refuses the token.
    enum class Outcome { Done, Stopped, Failed, Offline };

    void handle(const Message& message, const DocumentServer::Sink& reply);
    void run();
    void check();
    Outcome download(const NeededMedia& media);
    Outcome fetch(const NeededMedia& media, uint64_t size, const std::filesystem::path& part);
    // A stop when the window, the override or the shutdown forbids downloading now.
    std::optional<Outcome> forbidden();
    void setState(const std::string& state, const std::string& error = {});
    void setReadiness(Readiness readiness);
    // Waits up to `duration` or until the override changes; false when stopping.
    bool pause(std::chrono::duration<double> duration);

    DocumentServer& document_;
    MediaLibrary& library_;
    CompositionSource composition_;
    Now now_;
    MediaSyncOptions options_;
    std::function<void(const std::string&)> found_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool running_ = true;
    bool woken_ = false;
    // Counts override changes, which end a pause between chunks.
    int changes_ = 0;
    SyncOverride mode_ = SyncOverride::Auto;
    std::optional<Readiness> readiness_;
    std::string state_ = "checking";
    std::string error_;
    // The last error printed, so a retry that fails the same way stays quiet.
    std::string logged_;
    std::optional<NeededMedia> downloading_;
    uint64_t bytes_ = 0;
    uint64_t size_ = 0;
    std::thread thread_;
};

} // namespace azl
