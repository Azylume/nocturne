#include "supervisor/state_saver.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <utility>
#include <vector>

namespace azl {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kQuiet = std::chrono::duration<double>(StateSaver::kDebounce);
constexpr auto kMaxDelay = std::chrono::seconds(1);
constexpr auto kTick = std::chrono::seconds(1);
constexpr double kBeatTolerance = 0.01;

// Local time to the millisecond, so names sort in time order.
std::string stamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char text[32];
    const size_t n = std::strftime(text, sizeof text, "%Y%m%d-%H%M%S", &local);
    std::snprintf(text + n, sizeof text - n, ".%03lld", static_cast<long long>(ms));
    return text;
}

} // namespace

StateSaver::StateSaver(std::filesystem::path path, Now now, std::optional<Resume> last,
                       std::optional<std::string> source, std::optional<std::string> show)
    : path_(std::move(path)), now_(std::move(now)), source_(std::move(source)), show_(std::move(show)),
      resume_(std::move(last)),
      thread_([this] { run(); })
{
}

StateSaver::~StateSaver()
{
    {
        std::lock_guard lock(mutex_);
        running_ = false;
    }
    wake_.notify_all();
    thread_.join();
}

void StateSaver::changed(std::shared_ptr<const Composition> composition)
{
    std::lock_guard lock(mutex_);
    composition_ = std::move(composition);
    dirty_ = true;
    wake_.notify_all();
}

void StateSaver::fileChanged(const std::optional<ShowFile>& file)
{
    std::lock_guard lock(mutex_);
    if (file_ == file)
        return;
    file_ = file;
    dirty_ = true;
    wake_.notify_all();
}

uint64_t StateSaver::saves() const
{
    std::lock_guard lock(mutex_);
    return saves_;
}

bool StateSaver::drifted(const Resume& now) const
{
    if (!resume_)
        return true;
    return std::abs(resume_->beatAt(now.savedAt) - now.beat) > kBeatTolerance || resume_->tempo != now.tempo ||
           resume_->pilots != now.pilots || resume_->audio != now.audio;
}

uint64_t StateSaver::failures() const
{
    std::lock_guard lock(mutex_);
    return failures_;
}

void StateSaver::backup(const std::string& kind)
{
    std::lock_guard lock(backupMutex_);
    const std::filesystem::path dir = path_.parent_path() / "backups";
    const std::string suffix = "-" + kind + ".json";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!std::filesystem::copy_file(path_, dir / (stamp() + suffix), std::filesystem::copy_options::overwrite_existing,
                                    ec))
        return;
    std::vector<std::filesystem::path> kept;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.size() > suffix.size() && name.ends_with(suffix))
            kept.push_back(entry.path());
    }
    std::sort(kept.begin(), kept.end());
    for (size_t i = 0; i + kBackups < kept.size(); ++i)
        std::filesystem::remove(kept[i], ec);
}

void StateSaver::run()
{
    std::unique_lock lock(mutex_);
    while (true) {
        wake_.wait_for(lock, kTick, [this] { return dirty_ || !running_; });
        if (dirty_ && running_) {
            const auto first = Clock::now();
            while (running_) {
                dirty_ = false;
                const bool calm = !wake_.wait_for(lock, kQuiet, [this] { return dirty_ || !running_; });
                if (calm || Clock::now() - first >= kMaxDelay)
                    break;
            }
        }
        const bool stopping = !running_;
        std::shared_ptr<const Composition> composition = composition_;
        const std::optional<ShowFile> file = file_;
        dirty_ = false;
        lock.unlock();

        std::optional<Resume> now = now_ ? now_() : std::nullopt;
        const bool moved = now && drifted(*now);
        if (moved)
            resume_ = now;
        if (composition && (composition != saved_ || moved || file != savedFile_))
            save(composition, resume_, file);

        lock.lock();
        if (stopping)
            return;
    }
}

void StateSaver::save(const std::shared_ptr<const Composition>& composition, const std::optional<Resume>& resume,
                      const std::optional<ShowFile>& file)
{
    try {
        saveCompositionFile(path_, {*composition, resume, source_, show_, file});
        saved_ = composition;
        savedFile_ = file;
        if (!backedUp_ || Clock::now() - *backedUp_ >= std::chrono::duration<double>(kBackupEvery)) {
            backedUp_ = Clock::now();
            backup("minute");
        }
        std::lock_guard lock(mutex_);
        if (std::exchange(failing_, false))
            std::fprintf(stderr, "state: saved again to %s\n", path_.string().c_str());
        ++saves_;
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        ++failures_;
        if (!std::exchange(failing_, true))
            std::fprintf(stderr, "state: cannot save (%s); the show goes on, trying again every second\n", e.what());
    }
}

} // namespace azl
