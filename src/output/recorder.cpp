#include "output/recorder_session.hpp"

#include <cstdio>
#include <stdexcept>
#include <utility>

namespace azl {

Recorder::Recorder(Engine& engine, double fps, OnEnded onEnded, OnSpace onSpace)
    : engine_(engine), fps_(fps), onEnded_(std::move(onEnded)), onSpace_(std::move(onSpace)),
      space_(recording::volumeSpace)
{
}

Recorder::~Recorder()
{
    stop();
    std::lock_guard lock(control_);
    if (session_)
        session_->writer->join();
}

void Recorder::start(Settings settings, double now, std::optional<double> gigEnd)
{
    std::lock_guard control(control_);
    std::shared_ptr<Session> last;
    {
        std::lock_guard lock(mutex_);
        if (session_ && !session_->writer->done)
            throw std::runtime_error("already recording");
        last = session_;
    }
    if (last)
        last->writer->join();
    AudioPlan plan = audioPlan(settings.audio);
    AudioAnalysis* audio = nullptr;
    SpaceProvider space;
    std::chrono::milliseconds interval;
    {
        std::lock_guard lock(mutex_);
        audio = audio_;
        space = space_;
        interval = spaceInterval_;
    }
    std::shared_ptr<Session> session = makeSession(std::move(settings), std::move(plan), std::move(space), interval);
    recording::Writer& writer = *session->writer;
    if (writer.audioPlan.state == "unavailable")
        std::printf("record: no audio track: %s\n", writer.audioPlan.reason.c_str());
    writer.start(now, gigEnd, writeDelay_, finishDelay_);
    {
        std::lock_guard lock(mutex_);
        session_ = session;
    }
    if (writer.audioRate > 0.0) {
        session->analysis = audio;
        audio->setTap(session.get(), [session](const float* samples, size_t count, double time, double rate) {
            session->writer->heard(samples, count, time, rate);
        });
    }
    engine_.setFrameTap(session);
}

void Recorder::stop()
{
    std::lock_guard control(control_);
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(mutex_);
        if (!session_ || session_->writer->done)
            return;
        session = session_;
    }
    engine_.setFrameTap(nullptr);
    removeAudioTap(*session);
    session->writer->waitIdle(true);
    session->writer->close();
    session->writer->join();
}

void Recorder::ended(Session& session, const Ended& ended)
{
    {
        std::lock_guard lock(mutex_);
        if (session_.get() == &session) {
            engine_.setFrameTap(nullptr);
            removeAudioTap(session);
        }
        session.writer->done = true;
    }
    if (onEnded_)
        onEnded_(ended);
}

Recorder::Status Recorder::status() const
{
    std::lock_guard lock(mutex_);
    Status status;
    if (!session_)
        return status;
    const recording::Writer& writer = *session_->writer;
    status.recording = !writer.done;
    status.settings = writer.settings;
    status.file = writer.currentFile();
    status.frames = writer.counts.frames;
    status.dropped = writer.counts.dropped;
    status.hardware = writer.hardware;
    status.pixels = session_->pixels();
    status.audio = writer.audioPlan.state;
    status.audioReason = writer.audioPlan.reason;
    status.audioSource = writer.audioPlan.source;
    status.audioDropped = writer.audioDropped;
    status.space = writer.spaceLevel;
    status.freeBytes = writer.freeBytes;
    status.reserve = writer.reserve;
    status.stopsAt = writer.stopsAt;
    status.stopsFor = writer.stopsFor;
    return status;
}

std::optional<Recorder::Position> Recorder::position(double engineTime) const
{
    std::lock_guard lock(mutex_);
    if (!session_ || session_->writer->done)
        return std::nullopt;
    return session_->writer->position(engineTime);
}

void Recorder::waitUntilWritten()
{
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(mutex_);
        session = session_;
    }
    if (!session)
        return;
    session->writer->waitIdle(false);
    // The encoder lets go of what it was handed some time later.
    session->waitForEncoder();
}

void Recorder::setAudio(AudioAnalysis* audio, AudioSourceOf source)
{
    std::lock_guard lock(mutex_);
    audio_ = audio;
    audioSource_ = std::move(source);
}

Recorder::AudioPlan Recorder::audioPlan(bool wanted) const
{
    if (!wanted)
        return {};
    AudioAnalysis* audio = nullptr;
    AudioSourceOf sourceOf;
    {
        std::lock_guard lock(mutex_);
        audio = audio_;
        sourceOf = audioSource_;
    }
    if (!audio)
        return {.state = "unavailable", .reason = "no audio analysis"};
    const AudioSource source = sourceOf ? sourceOf() : AudioSource{.name = "audio input"};
    if (source.name.empty())
        return {.state = "unavailable", .reason = source.reason.empty() ? "no audio input running" : source.reason};
    if (audio->sampleRate() <= 0.0)
        return {.state = "unavailable", .reason = "no audio input running"};
    return {.state = "on", .source = source.name};
}

void Recorder::setSpaceCheck(SpaceProvider provider, std::chrono::milliseconds interval)
{
    std::lock_guard lock(mutex_);
    space_ = provider ? std::move(provider) : SpaceProvider(recording::volumeSpace);
    spaceInterval_ = interval;
}

uint64_t Recorder::reserveOf(const Settings& settings, uint64_t total)
{
    return recording::reserveOf(settings, total);
}

void Recorder::removeAudioTap(Session& session)
{
    if (session.writer->audioRate > 0.0 && session.analysis)
        session.analysis->setTap(&session, nullptr);
}

void Recorder::setWriteDelay(std::chrono::microseconds delay)
{
    std::lock_guard lock(mutex_);
    writeDelay_ = delay;
    if (session_)
        session_->writer->delayMicros = delay.count();
}

void Recorder::setRolloverDelays(std::chrono::microseconds finish, std::chrono::microseconds encoderSetup)
{
    std::lock_guard lock(mutex_);
    finishDelay_ = finish;
    encoderSetup_ = encoderSetup;
}

void Recorder::recover(const std::filesystem::path& folder)
{
    recording::recover(folder, plays);
}

std::string Recorder::folderProblem(const std::filesystem::path& path)
{
    return recording::folderProblem(path);
}

std::filesystem::path Recorder::movieFile(const std::filesystem::path& path)
{
    return recording::movieFile(path);
}

} // namespace azl

