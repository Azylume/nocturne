#include "output/recording.hpp"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <stdexcept>

namespace azl::recording {

namespace {

constexpr uint64_t kAlmostFull = uint64_t(1) << 20;
// Bounds the audio waiting for the writer; more is dropped and counted.
constexpr double kAudioBacklogSeconds = 2.0;
// A capture timestamp this far past where the track has got to starts a gap; anything closer is clock jitter.
constexpr double kAudioGapSeconds = 0.05;
constexpr const char* kPartialSuffix = ".partial.mov";
// The writer checks the free space itself after this much movie, so a busy machine that delays the checking thread
// cannot let it write past the reserve.
constexpr double kSpaceCheckSeconds = 0.25;

std::filesystem::path numbered(const std::filesystem::path& file, int number)
{
    char suffix[16];
    std::snprintf(suffix, sizeof suffix, " %03d", number);
    return file.parent_path() / (file.stem().string() + suffix + file.extension().string());
}

std::filesystem::path partialOf(const std::filesystem::path& file)
{
    return file.parent_path() / ("." + file.stem().string() + kPartialSuffix);
}

std::string megabytes(uint64_t bytes)
{
    return std::to_string(bytes >> 20) + " MB";
}

// Renames a complete movie into place; removes the partial file of one that did not complete.
bool placeMovie(bool complete, const std::filesystem::path& partial, const std::filesystem::path& file)
{
    std::error_code error;
    if (complete) {
        std::filesystem::rename(partial, file, error);
        if (!error)
            return true;
    }
    std::filesystem::remove(partial, error);
    return false;
}

std::string timestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm local {};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%d %H.%M.%S", &local);
    return text;
}

std::filesystem::path moviesFolder()
{
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    return std::filesystem::path(home ? home : "C:\\") / "Videos" / "Nocturne";
#else
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : "/tmp") / "Movies" / "Nocturne";
#endif
}

bool writable(const std::filesystem::path& folder)
{
#ifdef _WIN32
    return ::_waccess(folder.c_str(), 2) == 0;
#else
    return ::access(folder.c_str(), W_OK) == 0;
#endif
}

} // namespace

// Converts the input's chunks to kTrackRate through a windowed-sinc low-pass below both rates' Nyquist, so a faster
// input does not alias. The stream is taken as continuous until the rate changes or a chunk's time leaves it.
class Writer::Resampler {
public:
    // Appends a chunk and returns the samples it completes, the first at `time` (input clock).
    void push(const float* samples, size_t count, double time, double rate, std::vector<float>& out, double& outTime)
    {
        out.clear();
        if (rate == kTrackRate) {
            rate_ = rate;
            out.assign(samples, samples + count);
            outTime = time;
            return;
        }
        if (rate != rate_ || std::abs(time - (anchor_ + double(base_ + int64_t(in_.size())) / rate_)) > kAudioGapSeconds)
            reset(rate, time);
        in_.insert(in_.end(), samples, samples + count);
        outTime = anchor_ + pos_ / rate_;
        const int64_t end = base_ + int64_t(in_.size());
        while (int64_t(std::floor(pos_)) + half_ < end) {
            out.push_back(at(pos_));
            pos_ += rate_ / kTrackRate;
        }
        const int64_t keep = int64_t(std::floor(pos_)) - half_;
        if (keep > base_) {
            in_.erase(in_.begin(), in_.begin() + (keep - base_));
            base_ = keep;
        }
    }

private:
    static constexpr int kZeroCrossings = 16;
    static constexpr int kSteps = 256;

    void reset(double rate, double time)
    {
        rate_ = rate;
        anchor_ = time;
        base_ = 0;
        pos_ = 0.0;
        in_.clear();
        const double cutoff = 0.95 * std::min(1.0, kTrackRate / rate);
        half_ = int(std::ceil(kZeroCrossings / cutoff));
        kernel_.assign(size_t(half_ * kSteps + 2), 0.0f);
        for (size_t i = 0; i + 1 < kernel_.size(); ++i) {
            const double x = double(i) / kSteps;
            const double sinc = x == 0.0 ? 1.0 : std::sin(M_PI * cutoff * x) / (M_PI * cutoff * x);
            const double w = x / half_;
            const double blackman = 0.42 + 0.5 * std::cos(M_PI * w) + 0.08 * std::cos(2.0 * M_PI * w);
            kernel_[i] = float(cutoff * sinc * blackman);
        }
    }

    float at(double pos) const
    {
        const int64_t centre = int64_t(std::floor(pos));
        double sum = 0.0;
        for (int64_t k = std::max(centre - half_ + 1, base_); k <= centre + half_; ++k) {
            const double d = std::abs(pos - double(k)) * kSteps;
            const auto i = size_t(d);
            if (i + 1 >= kernel_.size())
                continue;
            const double f = d - double(i);
            sum += double(in_[size_t(k - base_)]) * (kernel_[i] + f * (kernel_[i + 1] - kernel_[i]));
        }
        return float(sum);
    }

    double rate_ = 0.0;
    double anchor_ = 0.0;
    // The input index of in_[0], and of the next output sample.
    int64_t base_ = 0;
    double pos_ = 0.0;
    int half_ = 0;
    std::vector<float> in_;
    std::vector<float> kernel_;
};

Writer::Writer(Settings s, AudioPlan audio, SpaceProvider space, std::chrono::milliseconds spaceInterval,
               OnEnded onEnded, OnSpace onSpace)
    : settings(std::move(s)), audioRate(audio.state == "on" ? kTrackRate : 0.0), audioPlan(std::move(audio)),
      onEnded_(std::move(onEnded)), onSpace_(std::move(onSpace)), space_(std::move(space)),
      spaceInterval_(spaceInterval), resampler_(std::make_unique<Resampler>())
{
    base_ = movieFile(settings.path);
    if (base_.extension() != ".mov")
        throw std::runtime_error("recordings are .mov files");
    folder_ = base_.parent_path();
    file_ = settings.maxBytes || settings.maxSeconds > 0.0 ? numbered(base_, number_) : base_;
    if (const std::string problem = folderProblem(settings.path); !problem.empty())
        throw std::runtime_error(problem);
    std::error_code error;
    if (settings.path.empty())
        std::filesystem::create_directories(folder_, error);
    if (std::filesystem::exists(file_))
        throw std::runtime_error(file_.string() + " exists");
    const Space room = space_(folder_);
    const uint64_t floor = reserveOf(settings, room.total);
    if (room.free < floor)
        throw std::runtime_error("too little disk space: " + megabytes(room.free) + " free at " + folder_.string() +
                                 ", below the recording reserve of " + megabytes(floor));
}

Writer::~Writer()
{
    close();
    join();
    if (segment_.movie && segment_.movie->writing()) {
        segment_.movie->cancel();
        std::error_code error;
        std::filesystem::remove(segment_.partial, error);
    }
}

void Writer::open(const EncoderFactory& make, std::chrono::microseconds encoderSetup)
{
    encoderSetupMicros_ = encoderSetup.count();
    if (const int64_t stall = encoderSetupMicros_.load()) {
        std::lock_guard held(encoderHardware_);
        std::this_thread::sleep_for(std::chrono::microseconds(stall));
    }
    encoder_ = make(counts);
    hardware = encoder_->hardware();
    segment_ = openSegment(file_);
    if (audioRate > 0.0 && !segment_.movie->hasAudio()) {
        audioRate = 0.0;
        audioPlan.state = "unavailable";
        audioPlan.reason = "the movie writer refuses AAC at 48 kHz";
    }
}

void Writer::start(double now, std::optional<double> gigEnd, std::chrono::microseconds writeDelay,
                   std::chrono::microseconds finishDelay)
{
    delayMicros = writeDelay.count();
    finishDelayMicros_ = finishDelay.count();
    startedAt_ = now;
    if (settings.maxTotalSeconds > 0.0) {
        stopsAt = now + settings.maxTotalSeconds;
        stopsFor = "max length";
    }
    if (const double end = gigEnd.value_or(0.0) + settings.gigGraceSeconds; gigEnd && (!stopsAt || end < *stopsAt)) {
        stopsAt = end;
        stopsFor = "gig over";
    }
    watcher_ = std::thread([this] { watch(); });
    thread_ = std::thread([this] { run(); });
}

void Writer::join()
{
    if (thread_.joinable())
        thread_.join();
    if (watcher_.joinable())
        watcher_.join();
}

Writer::Segment Writer::openSegment(const std::filesystem::path& movie)
{
    Segment next;
    next.file = movie;
    next.partial = partialOf(movie);
    std::error_code error;
    std::filesystem::remove(next.partial, error);
    next.movie = encoder_->open(next.partial, audioRate);
    return next;
}

void Writer::begin()
{
    std::lock_guard lock(mutex_);
    inFlight_++;
}

void Writer::queued(Picture picture, double seconds, bool ok)
{
    std::lock_guard lock(mutex_);
    inFlight_--;
    if (ok && !closing_) {
        queue_.push_back({std::move(picture), seconds});
        wake_.notify_all();
    }
    idle_.notify_all();
}

void Writer::run()
{
    std::string reason = "stopped";
    prepare();
    for (;;) {
        std::optional<Item> item;
        std::deque<Heard> sound;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return !queue_.empty() || closing_ || (origin_ && !audio_.empty()); });
            if (queue_.empty() && (closing_ || !origin_))
                break;
            if (!queue_.empty()) {
                item = std::move(queue_.front());
                queue_.pop_front();
                if (!origin_)
                    origin_ = item->seconds;
            }
            sound.swap(audio_);
            audioQueued_ = 0;
            writing_ = true;
        }
        bool ok = true;
        bool rollOk = true;
        const bool due = item && stopDue(item->seconds);
        const bool full = item && !due && spaceCheckDue(item->seconds) && spaceFull();
        if (item && !due && !full) {
            const bool rolls = rollDue(item->seconds);
            rollOk = !rolls || roll(reason);
            ok = rollOk && write(*item, rolls);
        }
        if (item)
            item->picture.reset();
        for (const Heard& h : sound)
            ok = ok && writeAudio(h);
        {
            std::lock_guard lock(mutex_);
            if (item)
                written_ = item->seconds;
            writing_ = false;
            idle_.notify_all();
        }
        if (due) {
            reason = stopsFor;
            break;
        }
        if (full) {
            reason = "disk full";
            break;
        }
        if (!rollOk)
            break;
        if (!ok) {
            reason = segment_.movie->diskFull() || volumeSpace(folder_).free < kAlmostFull
                         ? "disk full"
                         : "failed: " + segment_.movie->error();
            break;
        }
    }
    {
        std::lock_guard lock(mutex_);
        if (!closedFor_.empty())
            reason = closedFor_;
    }
    finish(reason);
    watcher_.join();
}

bool Writer::rollDue(double seconds) const
{
    if (segment_.frames == 0)
        return false;
    if (settings.maxSeconds > 0.0 && seconds - *segment_.origin >= settings.maxSeconds - 1e-6)
        return true;
    if (settings.maxBytes == 0)
        return false;
    if (const std::optional<uint64_t> bytes = segment_.movie->bytes())
        return *bytes >= settings.maxBytes;
    std::error_code error;
    const uintmax_t size = std::filesystem::file_size(segment_.partial, error);
    return !error && size >= settings.maxBytes;
}

bool Writer::spaceCheckDue(double seconds)
{
    if (spaceCheckedAt_ && seconds - *spaceCheckedAt_ < kSpaceCheckSeconds)
        return false;
    spaceCheckedAt_ = seconds;
    return true;
}

bool Writer::stopDue(double seconds) const
{
    return stopsAt && startedAt_ + (seconds - *origin_) >= *stopsAt - 1e-6;
}

// With a maximum size or length, opens the next numbered file in the background, so a rollover only swaps files.
void Writer::prepare()
{
    if (!settings.maxBytes && settings.maxSeconds <= 0.0)
        return;
    prepared_ = std::async(std::launch::async, [this, next = numbered(base_, number_ + 1)] {
        if (std::filesystem::exists(next))
            throw std::runtime_error(next.string() + " exists");
        return openSegment(next);
    });
}

bool Writer::roll(std::string& reason)
{
    const auto began = std::chrono::steady_clock::now();
    Segment opened;
    try {
        opened = prepared_.get();
    } catch (const std::exception& e) {
        reason = std::string("failed: ") + e.what();
        return false;
    }
    const uint64_t droppedNow = counts.dropped;
    std::printf("record: %s is full (%llu frames, %llu dropped), going on in %s after %.1f ms\n",
                segment_.file.string().c_str(), (unsigned long long)segment_.frames,
                (unsigned long long)(droppedNow - segment_.droppedBefore), opened.file.string().c_str(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
    opened.droppedBefore = droppedNow;
    retire(std::exchange(segment_, std::move(opened)));
    {
        std::lock_guard lock(mutex_);
        number_++;
        file_ = segment_.file;
    }
    prepare();
    return true;
}

// Finishes a full segment on a thread of its own; the movie is renamed into place once complete.
void Writer::retire(Segment old)
{
    const int64_t delay = finishDelayMicros_.load();
    retiring_.push_back(std::async(std::launch::async, [delay, old = std::move(old)] {
        if (delay)
            std::this_thread::sleep_for(std::chrono::microseconds(delay));
        if (!placeMovie(old.movie->finish(), old.partial, old.file))
            std::printf("record: could not finish %s: %s\n", old.file.string().c_str(), old.movie->error().c_str());
    }));
}

// The file opened for a rollover that never came.
void Writer::discardPrepared()
{
    if (!prepared_.valid())
        return;
    try {
        Segment spare = prepared_.get();
        spare.movie->cancel();
        std::error_code error;
        std::filesystem::remove(spare.partial, error);
    } catch (const std::exception&) {
    }
}

// Checks the free space every interval until the recording closes, and closes it below the reserve.
void Writer::watch()
{
    std::unique_lock lock(mutex_);
    while (!closing_) {
        wake_.wait_for(lock, spaceInterval_, [&] { return closing_; });
        if (closing_)
            return;
        lock.unlock();
        const bool full = spaceFull();
        lock.lock();
        if (full) {
            closedFor_ = "disk full";
            closing_ = true;
            wake_.notify_all();
            idle_.notify_all();
        }
    }
}

bool Writer::spaceFull()
{
    const Space room = space_(folder_);
    const uint64_t floor = reserveOf(settings, room.total);
    const SpaceLevel level = room.free < floor       ? SpaceLevel::Full
                             : room.free / 2 < floor ? SpaceLevel::Low
                                                     : SpaceLevel::Ok;
    freeBytes = room.free;
    reserve = floor;
    const SpaceLevel last = spaceLevel.exchange(level);
    if (level == last || (last == SpaceLevel::Unknown && level == SpaceLevel::Ok))
        return level == SpaceLevel::Full;
    const std::string where = folder_.string();
    if (level == SpaceLevel::Full)
        std::printf("record: %s free at %s, below the reserve of %s: stopping\n", megabytes(room.free).c_str(),
                    where.c_str(), megabytes(floor).c_str());
    else if (level == SpaceLevel::Low)
        std::printf("record: disk space low, %s free at %s; the recording stops below %s\n",
                    megabytes(room.free).c_str(), where.c_str(), megabytes(floor).c_str());
    else
        std::printf("record: disk space back to %s free at %s\n", megabytes(room.free).c_str(), where.c_str());
    if (onSpace_)
        onSpace_();
    return level == SpaceLevel::Full;
}

bool Writer::write(const Item& item, bool keyFrame)
{
    if (const int64_t delay = delayMicros.load())
        std::this_thread::sleep_for(std::chrono::microseconds(delay));
    if (counts.failed)
        return false;
    if (!segment_.origin) {
        segment_.origin = item.seconds;
        std::lock_guard lock(mutex_);
        starts_.emplace_back(segment_.file, item.seconds);
    }
    std::unique_lock<std::mutex> held;
    if (encoderSetupMicros_.load())
        held = std::unique_lock(encoderHardware_);
    if (!encoder_->encode(item.picture, item.seconds, *segment_.origin, keyFrame, *segment_.movie))
        return false;
    segment_.frames++;
    return true;
}

void Writer::heard(const float* samples, size_t count, double time, double sampleRate)
{
    std::lock_guard lock(mutex_);
    if (closing_ || sampleRate <= 0.0)
        return;
    if (double(audioQueued_ + count) > kAudioBacklogSeconds * sampleRate) {
        audioDropped += count;
        return;
    }
    audio_.push_back({std::vector<float>(samples, samples + count), time, sampleRate});
    audioQueued_ += count;
    wake_.notify_all();
}

bool Writer::writeAudio(const Heard& heard)
{
    if (!segment_.movie->hasAudio() || !segment_.origin)
        return true;
    double time = 0.0;
    resampler_->push(heard.samples.data(), heard.samples.size(), heard.time, heard.rate, resampled_, time);
    return resampled_.empty() || placeAudio(resampled_, time);
}

// Places samples at their capture time on the movie's clock, the one the frames are on, and keeps the track continuous
// through timestamp jitter.
bool Writer::placeAudio(const std::vector<float>& samples, double time)
{
    int64_t& audioNext = segment_.audioNext;
    const auto start = int64_t(std::llround((time - *segment_.origin) * audioRate));
    if (audioNext < 0 || start > audioNext + int64_t(kAudioGapSeconds * audioRate))
        audioNext = std::max<int64_t>(start, 0);
    const int64_t skip = audioNext - start;
    if (skip >= int64_t(samples.size()))
        return true;
    const size_t first = size_t(std::max<int64_t>(skip, 0));
    const size_t count = samples.size() - first;
    if (!segment_.movie->audioReady()) {
        audioDropped += count;
        audioNext += int64_t(count);
        return true;
    }
    const bool ok = segment_.movie->appendAudio(samples.data() + first, count, audioNext);
    audioNext += int64_t(count);
    return ok;
}

void Writer::finish(std::string reason)
{
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
        queue_.clear();
        audio_.clear();
        idle_.notify_all();
        wake_.notify_all();
    }
    encoder_->complete();
    MovieFile& movie = *segment_.movie;
    bool kept = false;
    if (movie.writing() && segment_.frames > 0) {
        if (!movie.finish())
            reason = movie.diskFull() ? "disk full" : "failed: " + movie.error();
        else if (!(kept = placeMovie(true, segment_.partial, segment_.file)))
            reason = "failed: cannot rename the movie into place";
    } else if (movie.writing()) {
        movie.cancel();
    }
    std::error_code error;
    if (!kept)
        std::filesystem::remove(segment_.partial, error);
    discardPrepared();
    for (std::future<void>& retired : retiring_)
        retired.wait();
    encoder_.reset();
    onEnded_({segment_.file, counts.frames.load(), counts.dropped.load(), reason, kept});
}

void Writer::close()
{
    std::lock_guard lock(mutex_);
    closing_ = true;
    wake_.notify_all();
}

void Writer::waitIdle(bool inFlightOnly)
{
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] {
        return inFlight_ == 0 && (inFlightOnly || closing_ || (queue_.empty() && (audio_.empty() || !origin_) && !writing_));
    });
}

std::filesystem::path Writer::currentFile() const
{
    std::lock_guard lock(mutex_);
    return file_;
}

std::optional<Position> Writer::position(double engineTime) const
{
    std::lock_guard lock(mutex_);
    if (starts_.empty())
        return Position{file_, 0.0, false};
    auto in = std::find_if(starts_.rbegin(), starts_.rend(), [&](const auto& s) { return s.second <= engineTime; });
    const auto& start = in == starts_.rend() ? starts_.front() : *in;
    const bool settled = written_ && *written_ >= engineTime;
    return Position{start.first, std::max(0.0, engineTime - start.second), settled};
}

uint64_t reserveOf(const Settings& settings, uint64_t total)
{
    return std::max(settings.reserveBytes, uint64_t(double(total) * settings.reservePercent / 100.0));
}

Space volumeSpace(const std::filesystem::path& folder)
{
    std::error_code error;
    const std::filesystem::space_info info = std::filesystem::space(folder, error);
    if (error)
        return {};
    return {uint64_t(info.available), uint64_t(info.capacity)};
}

void recover(const std::filesystem::path& folder, const std::function<bool(const std::filesystem::path&)>& plays)
{
    const std::string suffix = kPartialSuffix;
    std::vector<std::filesystem::path> partials;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        const std::string name = entry.path().filename().string();
        if (name.size() > suffix.size() + 1 && name[0] == '.' && name.ends_with(suffix))
            partials.push_back(entry.path());
    }
    for (const std::filesystem::path& partial : partials) {
        const std::string name = partial.filename().string();
        const std::string stem = name.substr(1, name.size() - suffix.size() - 1);
        std::filesystem::path file = folder / (stem + ".mov");
        if (std::filesystem::exists(file))
            file = folder / (stem + " recovered.mov");
        bool moved = plays(partial) && !std::filesystem::exists(file);
        if (moved) {
            std::filesystem::rename(partial, file, error);
            moved = !error;
        }
        if (!moved) {
            std::printf("record: cannot recover %s, left as it is\n", partial.string().c_str());
            continue;
        }
        std::printf("record: recovered the unfinished recording %s\n", file.string().c_str());
    }
}

std::string folderProblem(const std::filesystem::path& path)
{
    if (path.empty())
        return {};
    const std::filesystem::path folder = path.has_extension() ? path.parent_path() : path;
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error))
        return "the recording folder " + folder.string() + " does not exist (is its disk connected?)";
    if (!writable(folder))
        return "cannot write to the recording folder " + folder.string();
    return {};
}

std::filesystem::path movieFile(const std::filesystem::path& path)
{
    std::filesystem::path base = path.empty() ? moviesFolder() : path;
    if (base.has_extension())
        return base;
    return base / ("Nocturne " + timestamp() + ".mov");
}

void checkCodec(const std::string& codec)
{
    if (codec != "hevc" && codec != "prores")
        throw std::runtime_error("unknown codec " + codec + " (hevc or prores)");
}

} // namespace azl::recording
