#include "ui/client/director_media.hpp"

#include "media/library_service.hpp"
#include "media/stills.hpp"
#include "upload/media_upload.hpp"

#include <cstdio>

namespace azl::ui {

namespace fs = std::filesystem;

namespace {

bool inside(const fs::path& file, const fs::path& folder)
{
    if (folder.empty())
        return false;
    const fs::path relative = file.lexically_normal().lexically_relative(folder.lexically_normal());
    return !relative.empty() && *relative.begin() != "..";
}

} // namespace

DirectorMedia::Options DirectorMedia::defaults()
{
    return {.library = MediaLibrary::defaultRoot(),
            .keepLocal = defaultKeepLocalFile(),
            .stills = defaultStillsFolder(),
            .fetched = defaultFetchFolder(),
            .bucket = [] { return std::make_shared<S3Client>(loadR2Config(defaultR2ConfigPath())); },
            .uploads = defaultUploadStatesFile()};
}

DirectorMedia::DirectorMedia(Options options, std::function<void(const std::string&)> changed)
    : options_(std::move(options)), changed_(std::move(changed)),
      library_(options_.library.empty() ? nullptr : std::make_unique<MediaLibrary>(options_.library)),
      keep_(options_.keepLocal), uploads_(options_.uploads), stills_(options_.stills),
      renders_("nocturne.ui.stills", ThreadRole::Background)
{
    if (library_ && options_.bucket && !options_.fetched.empty())
        fetcher_ = std::make_unique<MediaFetcher>(
            options_.bucket, options_.fetched,
            [this](const std::string& hash, const fs::path& file) { arrived(hash, file); },
            [this](const std::string& hash) { changed_(hash); });
}

DirectorMedia::~DirectorMedia()
{
    fetcher_.reset();
    renders_.flush();
}

bool DirectorMedia::has(const std::string& hash)
{
    if (!library_)
        return false;
    {
        std::lock_guard lock(mutex_);
        if (auto it = present_.find(hash); it != present_.end())
            return it->second;
    }
    const std::optional<MediaEntry> entry = library_->find(hash);
    const bool present = entry && entry->status == MediaStatus::Ok;
    std::lock_guard lock(mutex_);
    present_[hash] = present;
    return present;
}

std::optional<std::vector<uint8_t>> DirectorMedia::still(const std::string& hash)
{
    {
        std::lock_guard lock(mutex_);
        if (auto it = rendered_.find(hash); it != rendered_.end())
            return it->second;
        if (rendering_.contains(hash))
            return std::vector<uint8_t>{};
    }
    if (std::optional<std::vector<uint8_t>> cached = stills_.find(hash)) {
        std::lock_guard lock(mutex_);
        rendered_[hash] = *cached;
        return cached;
    }
    if (!has(hash))
        return std::nullopt;
    renderStill(hash);
    return std::vector<uint8_t>{};
}

void DirectorMedia::renderStill(const std::string& hash)
{
    const std::optional<MediaEntry> entry = library_->find(hash);
    if (!entry)
        return;
    {
        std::lock_guard lock(mutex_);
        if (!rendering_.insert(hash).second)
            return;
    }
    renders_.async([this, hash, file = entry->path] {
        std::vector<uint8_t> jpeg;
        try {
            jpeg = videoStill(file, kStillWidth);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "still of %s: %s\n", file.string().c_str(), e.what());
        }
        stills_.store(hash, jpeg);
        {
            std::lock_guard lock(mutex_);
            rendering_.erase(hash);
            rendered_[hash] = std::move(jpeg);
        }
        changed_(hash);
    });
}

void DirectorMedia::storeStill(const std::string& hash, const std::vector<uint8_t>& jpeg)
{
    stills_.store(hash, jpeg);
    std::lock_guard lock(mutex_);
    rendered_[hash] = jpeg;
}

void DirectorMedia::setClip(const std::string& hash, std::optional<bool> keep)
{
    keep_.setClip(hash, keep);
}

void DirectorMedia::setShow(const std::string& show, bool keep, const std::set<std::string>& media)
{
    keep_.setShow(show, keep, media);
}

void DirectorMedia::reconcile(const std::string& show, const std::set<std::string>& media)
{
    if (!show.empty())
        keep_.updateShow(show, media);
    if (!library_)
        return;
    for (const MediaEntry& entry : library_->entries()) {
        const bool fetched = inside(entry.path, options_.fetched);
        if (fetched && !keep_.chosen(entry.hash))
            remove(entry.hash, true);
        else if (!fetched && keep_.clip(entry.hash) == false)
            remove(entry.hash, false);
    }
    if (!fetcher_)
        return;
    for (const std::string& hash : keep_.chosen())
        if (!has(hash))
            fetcher_->want(hash);
}

void DirectorMedia::remove(const std::string& hash, bool deleteFile)
{
    if (fetcher_)
        fetcher_->cancel(hash);
    const std::optional<MediaEntry> entry = library_->remove(hash);
    if (entry && deleteFile) {
        std::error_code ec;
        fs::remove(entry->path, ec);
    }
    setPresent(hash, false);
}

void DirectorMedia::arrived(const std::string& hash, const fs::path& file)
{
    const MediaEntry entry = library_->add(file);
    if (entry.hash != hash)
        std::fprintf(stderr, "fetched %s holds %s\n", hash.c_str(), entry.hash.c_str());
    setPresent(entry.hash, entry.status == MediaStatus::Ok);
}

void DirectorMedia::setPresent(const std::string& hash, bool present)
{
    {
        std::lock_guard lock(mutex_);
        present_[hash] = present;
    }
    changed_(hash);
}

DirectorMedia::Where DirectorMedia::where(const std::string& hash)
{
    if (has(hash))
        return Where::Local;
    const std::optional<MediaFetcher::State> state = fetcher_ ? fetcher_->state(hash) : std::nullopt;
    if (!state)
        return Where::Remote;
    switch (*state) {
    case MediaFetcher::State::Queued: return Where::Queued;
    case MediaFetcher::State::Fetching: return Where::Fetching;
    case MediaFetcher::State::Failed: return Where::Failed;
    }
    return Where::Remote;
}

std::string DirectorMedia::fetchError(const std::string& hash) const
{
    return fetcher_ ? fetcher_->error(hash) : std::string();
}

DirectorMedia::Upload DirectorMedia::upload(const std::string& hash)
{
    const std::optional<UploadState> state = uploads_.find(hash);
    if (!state)
        return has(hash) ? Upload::LocalOnly : Upload::Unknown;
    switch (state->kind) {
    case UploadState::Kind::Uploading: return Upload::Uploading;
    case UploadState::Kind::Uploaded: return Upload::Uploaded;
    case UploadState::Kind::Failed: return Upload::Failed;
    }
    return Upload::Unknown;
}

} // namespace azl::ui
