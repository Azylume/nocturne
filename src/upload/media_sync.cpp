#include "upload/media_sync.hpp"

#include "document/composition_file.hpp"
#include "media/keep_local.hpp"
#include "upload/media_upload.hpp"

#include <pthread/qos.h>
#include <sys/resource.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace azl {

namespace {

constexpr const char* kChannel = "sync";

SyncOverride loadMode(const fs::path& file)
{
    std::ifstream in(file);
    if (!in)
        return SyncOverride::Auto;
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_object() && j.value("override", Message()).is_string())
        if (const auto mode = overrideFromName(j["override"].get<std::string>()))
            return *mode;
    std::fprintf(stderr, "media sync: unreadable %s, syncing by the Gig window\n", file.c_str());
    return SyncOverride::Auto;
}

std::string extensionOf(const std::string& name)
{
    const std::string extension = mediaExtension(name);
    return extension.empty() ? ".mp4" : extension;
}

std::string shortName(const NeededMedia& media)
{
    return (media.name.empty() ? std::string("a clip") : media.name) + " (" + media.hash.substr(0, 12) + ")";
}

} // namespace

std::vector<NeededMedia> neededMedia(const Composition& composition)
{
    std::map<std::string, NeededMedia> needed;
    forEachLayer(composition.layers, [&](const Layer& layer) {
        for (const auto& [column, clip] : layer.clips) {
            if (clip.hash.empty())
                continue;
            NeededMedia& media = needed[clip.hash];
            media.hash = clip.hash;
            if (media.name.empty())
                media.name = clip.name;
            media.decks.insert(columnDeck(composition, column));
        }
    });
    std::vector<NeededMedia> list;
    for (auto& [hash, media] : needed)
        list.push_back(std::move(media));
    return list;
}

Readiness readiness(const Composition& composition, const std::vector<NeededMedia>& needed,
                    const std::set<std::string>& present)
{
    Readiness r{.needed = int(needed.size())};
    std::map<uint64_t, int> byDeck;
    for (const NeededMedia& media : needed) {
        if (present.contains(media.hash))
            continue;
        r.missing++;
        for (uint64_t deck : media.decks)
            byDeck[deck]++;
    }
    for (const Deck& deck : composition.decks)
        if (const auto it = byDeck.find(deck.id); it != byDeck.end())
            r.decks.push_back({deck.id, deck.name, it->second});
    return r;
}

const char* overrideName(SyncOverride mode)
{
    switch (mode) {
    case SyncOverride::Auto: return "auto";
    case SyncOverride::Allow: return "allow";
    case SyncOverride::Block: return "block";
    }
    return "auto";
}

std::optional<SyncOverride> overrideFromName(const std::string& name)
{
    for (SyncOverride mode : {SyncOverride::Auto, SyncOverride::Allow, SyncOverride::Block})
        if (name == overrideName(mode))
            return mode;
    return std::nullopt;
}

bool syncAllowed(const std::vector<GigWindow>& gigs, double now, SyncOverride mode, const UtcOffset& zone)
{
    if (mode != SyncOverride::Auto)
        return mode == SyncOverride::Allow;
    return !inGigWindow(gigs, now, kChannel, zone);
}

std::optional<std::string> readinessAlert(const std::vector<GigWindow>& gigs, double now, const Readiness& readiness,
                                          const UtcOffset& zone)
{
    if (readiness.ready())
        return std::nullopt;
    for (const GigWindow& window : gigs) {
        const auto [start, end] = gigSpan(window, zone);
        if (now < double(start) - kReadinessAlertBefore || now >= double(end))
            continue;
        return now < double(start) ? "media not ready, gig at " + clockText(window.start)
                                   : std::string("media not ready, gig under way");
    }
    return std::nullopt;
}

S3Config loadVenueR2Config()
{
    return loadR2Config(defaultR2ConfigPath().parent_path() / "r2-read.json", "nocturne-r2-read", "NOCTURNE_R2_READ");
}

MediaSync::MediaSync(DocumentServer& document, MediaLibrary& library, CompositionSource composition, Now now,
                     MediaSyncOptions options, std::function<void(const std::string&)> found)
    : document_(document), library_(library), composition_(std::move(composition)), now_(std::move(now)),
      options_(std::move(options)), found_(std::move(found))
{
    if (!options_.settings.empty())
        mode_ = loadMode(options_.settings);
    document_.handle("mediaSync", [this](const Message& message, DocumentServer::Sink reply) { handle(message, reply); });
    thread_ = std::thread([this] { run(); });
}

MediaSync::~MediaSync()
{
    document_.handle("mediaSync", nullptr);
    {
        std::lock_guard lock(mutex_);
        running_ = false;
    }
    wake_.notify_all();
    thread_.join();
}

Message MediaSync::status() const
{
    const std::shared_ptr<const Composition> composition = composition_();
    const double now = now_();
    std::lock_guard lock(mutex_);
    Message decks = Message::array();
    std::optional<std::string> alert;
    if (readiness_) {
        for (const DeckMissing& d : readiness_->decks)
            decks.push_back({{"deck", d.deck}, {"name", d.name}, {"missing", d.missing}});
        alert = readinessAlert(composition->gigs, now, *readiness_, options_.zone);
    }
    Message download = nullptr;
    if (downloading_)
        download = {{"hash", downloading_->hash}, {"name", downloading_->name}, {"bytes", bytes_}, {"size", size_}};
    return {{"ready", readiness_ && readiness_->ready()},
            {"needed", readiness_ ? Message(readiness_->needed) : Message(nullptr)},
            {"missing", readiness_ ? Message(readiness_->missing) : Message(nullptr)},
            {"decks", decks},
            {"override", overrideName(mode_)},
            {"inWindow", inGigWindow(composition->gigs, now, kChannel, options_.zone)},
            {"allowed", syncAllowed(composition->gigs, now, mode_, options_.zone)},
            {"state", state_},
            {"download", download},
            {"error", error_.empty() ? Message(nullptr) : Message(error_)},
            {"alert", alert ? Message(*alert) : Message(nullptr)}};
}

SyncOverride MediaSync::mode() const
{
    std::lock_guard lock(mutex_);
    return mode_;
}

void MediaSync::setMode(SyncOverride mode)
{
    if (!options_.settings.empty()) {
        fs::create_directories(options_.settings.parent_path());
        writeFileAtomically(options_.settings, nlohmann::json{{"override", overrideName(mode)}}.dump() + "\n");
    }
    {
        std::lock_guard lock(mutex_);
        mode_ = mode;
        changes_++;
        woken_ = true;
    }
    wake_.notify_all();
}

void MediaSync::wake()
{
    {
        std::lock_guard lock(mutex_);
        woken_ = true;
    }
    wake_.notify_all();
}

void MediaSync::handle(const Message& message, const DocumentServer::Sink& reply)
{
    const std::string op = message.value("op", "");
    auto statusMessage = [this] {
        Message m = status();
        m["type"] = "mediaSync";
        m["op"] = "status";
        return m;
    };
    if (op == "status")
        return reply(statusMessage());
    if (op != "set")
        return reply({{"type", "error"}, {"message", "unknown mediaSync op " + op}});
    const std::optional<SyncOverride> mode =
        message.value("override", Message()).is_string() ? overrideFromName(message["override"].get<std::string>())
                                                         : std::nullopt;
    if (!mode)
        return reply({{"type", "error"}, {"message", "mediaSync set needs \"override\": auto, allow or block"}});
    try {
        setMode(*mode);
    } catch (const std::exception& e) {
        return reply({{"type", "error"}, {"message", std::string("cannot keep the media sync override: ") + e.what()}});
    }
    document_.broadcast(statusMessage());
}

void MediaSync::run()
{
    pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
    setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_THREAD, IOPOL_THROTTLE);
    std::unique_lock lock(mutex_);
    while (running_) {
        woken_ = false;
        lock.unlock();
        check();
        lock.lock();
        const auto wait = state_ == "error" ? options_.retry : options_.every;
        wake_.wait_for(lock, wait, [this] { return !running_ || woken_; });
    }
}

void MediaSync::check()
{
    const std::shared_ptr<const Composition> composition = composition_();
    const std::vector<NeededMedia> needed = neededMedia(*composition);
    std::set<std::string> known;
    for (const MediaEntry& entry : library_.entries())
        known.insert(entry.hash);
    std::set<std::string> present;
    std::vector<const NeededMedia*> missing;
    for (const NeededMedia& media : needed) {
        const std::optional<MediaEntry> entry = known.contains(media.hash) ? library_.find(media.hash) : std::nullopt;
        if (entry && entry->status != MediaStatus::Missing)
            present.insert(media.hash);
        else
            missing.push_back(&media);
    }
    setReadiness(readiness(*composition, needed, present));
    if (missing.empty())
        return setState("ready");
    if (!options_.client)
        return setState("error", options_.clientError);

    std::string failed;
    for (const NeededMedia* media : missing) {
        const Outcome outcome = download(*media);
        if (outcome == Outcome::Stopped || outcome == Outcome::Offline)
            return;
        if (outcome == Outcome::Failed) {
            std::lock_guard lock(mutex_);
            failed = error_;
            continue;
        }
        present.insert(media->hash);
        setReadiness(readiness(*composition, needed, present));
    }
    if (failed.empty())
        setState("ready");
    else
        setState("error", failed);
}

std::optional<MediaSync::Outcome> MediaSync::forbidden()
{
    const std::shared_ptr<const Composition> composition = composition_();
    const double now = now_();
    std::lock_guard lock(mutex_);
    if (!running_)
        return Outcome::Stopped;
    if (syncAllowed(composition->gigs, now, mode_, options_.zone))
        return std::nullopt;
    downloading_.reset();
    const std::string state = mode_ == SyncOverride::Block ? "blocked" : "waiting";
    if (state_ != state)
        std::printf("media sync: %s\n", state == "blocked" ? "blocked by hand" : "waiting for the Gig window to end");
    state_ = state;
    error_.clear();
    return Outcome::Stopped;
}

MediaSync::Outcome MediaSync::download(const NeededMedia& media)
{
    if (const auto stop = forbidden())
        return *stop;
    if (!isMediaHash(media.hash)) {
        setState("error", shortName(media) + " has no media hash");
        return Outcome::Failed;
    }
    setState("downloading");
    try {
        const std::optional<uint64_t> size = options_.client->head(media.hash);
        if (!size) {
            setState("error", shortName(media) + " is not in the bucket");
            return Outcome::Failed;
        }
        fs::create_directories(options_.folder);
        const fs::path part = options_.folder / (media.hash + ".part");
        for (int attempt = 1; attempt <= options_.attempts; ++attempt) {
            if (const Outcome outcome = fetch(media, *size, part); outcome != Outcome::Done)
                return outcome;
            if (sha256File(part) == media.hash) {
                const fs::path file = options_.folder / (media.hash + extensionOf(media.name));
                fs::rename(part, file);
                library_.add(file);
                {
                    std::lock_guard lock(mutex_);
                    downloading_.reset();
                }
                std::printf("media sync: %s arrived and verified\n", shortName(media).c_str());
                if (found_)
                    found_(media.hash);
                return Outcome::Done;
            }
            fs::remove(part);
            std::printf("media sync: %s arrived with another hash, fetching it again\n", shortName(media).c_str());
        }
        setState("error", shortName(media) + " arrived with another hash " + std::to_string(options_.attempts) +
                              " times");
        return Outcome::Failed;
    } catch (const TransportError& e) {
        setState("error", std::string("connection lost: ") + e.what());
        return Outcome::Offline;
    } catch (const S3Error& e) {
        setState("error", e.what());
        return e.status == 401 || e.status == 403 ? Outcome::Offline : Outcome::Failed;
    } catch (const std::exception& e) {
        setState("error", shortName(media) + ": " + e.what());
        return Outcome::Failed;
    }
}

MediaSync::Outcome MediaSync::fetch(const NeededMedia& media, uint64_t size, const fs::path& part)
{
    std::error_code ec;
    uint64_t offset = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;
    if (ec || offset > size) {
        fs::remove(part, ec);
        offset = 0;
    }
    {
        std::lock_guard lock(mutex_);
        downloading_ = media;
        bytes_ = offset;
        size_ = size;
    }
    std::ofstream out(part, std::ios::binary | std::ios::app);
    const auto start = std::chrono::steady_clock::now();
    uint64_t fetched = 0;
    while (offset < size) {
        if (const auto stop = forbidden())
            return *stop;
        const std::optional<std::string> chunk =
            options_.client->getRange(media.hash, offset, std::min(options_.chunk, size - offset));
        if (!chunk || chunk->empty())
            throw S3Error(404, "NoSuchKey", shortName(media) + " is gone from the bucket");
        out.write(chunk->data(), std::streamsize(chunk->size()));
        out.flush();
        if (!out)
            throw std::runtime_error("cannot write " + part.string());
        offset += chunk->size();
        fetched += chunk->size();
        {
            std::lock_guard lock(mutex_);
            bytes_ = offset;
        }
        if (options_.bytesPerSecond <= 0)
            continue;
        const std::chrono::duration<double> ahead =
            std::chrono::duration<double>(double(fetched) / options_.bytesPerSecond) -
            (std::chrono::steady_clock::now() - start);
        if (ahead.count() > 0 && !pause(ahead))
            return Outcome::Stopped;
    }
    return Outcome::Done;
}

void MediaSync::setState(const std::string& state, const std::string& error)
{
    std::lock_guard lock(mutex_);
    if (state == "error" && error != logged_)
        std::printf("media sync: %s\n", error.c_str());
    if (state == "error" || state == "ready")
        logged_ = error;
    state_ = state;
    error_ = error;
    if (state != "downloading")
        downloading_.reset();
}

void MediaSync::setReadiness(Readiness readiness)
{
    std::lock_guard lock(mutex_);
    readiness_ = std::move(readiness);
}

bool MediaSync::pause(std::chrono::duration<double> duration)
{
    std::unique_lock lock(mutex_);
    const int changes = changes_;
    wake_.wait_for(lock, duration, [&] { return !running_ || changes_ != changes; });
    return running_;
}

} // namespace azl
