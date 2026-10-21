#include "output/record_service.hpp"

#include "document/composition_file.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <strings.h>
#include <unistd.h>

namespace azl {

namespace {

const char* spaceName(Recorder::SpaceLevel level)
{
    switch (level) {
    case Recorder::SpaceLevel::Ok: return "ok";
    case Recorder::SpaceLevel::Low: return "low";
    case Recorder::SpaceLevel::Full: return "full";
    case Recorder::SpaceLevel::Unknown: break;
    }
    return "unknown";
}

bool audioSetting(const Message& value)
{
    if (value.is_string())
        return value.get<std::string>() != "off";
    return value.get<bool>();
}

Recorder::Settings applied(const Message& message, Recorder::Settings next)
{
    if (auto target = message.find("target"); target != message.end())
        next.target = target->get<std::string>();
    if (auto codec = message.find("codec"); codec != message.end())
        next.codec = codec->get<std::string>();
    if (auto path = message.find("path"); path != message.end())
        next.path = path->get<std::string>();
    next.width = message.value("width", next.width);
    next.height = message.value("height", next.height);
    if (auto audio = message.find("audio"); audio != message.end())
        next.audio = audioSetting(*audio);
    next.precise = message.value("precise", next.precise);
    if (auto reserve = message.find("reserveMB"); reserve != message.end())
        next.reserveBytes = reserve->get<uint64_t>() << 20;
    next.reservePercent = message.value("reservePercent", next.reservePercent);
    if (auto maxMB = message.find("maxMB"); maxMB != message.end())
        next.maxBytes = maxMB->get<uint64_t>() << 20;
    next.maxSeconds = message.value("maxSeconds", next.maxSeconds);
    next.maxTotalSeconds = message.value("maxTotalSeconds", next.maxTotalSeconds);
    next.gigGraceSeconds = message.value("gigGraceSeconds", next.gigGraceSeconds);
    if (next.reservePercent < 0.0 || next.reservePercent > 90.0)
        throw std::runtime_error("the reserve is 0 to 90 % of the disk");
    if (next.maxSeconds < 0.0)
        throw std::runtime_error("the maximum length is 0 (none) or more seconds");
    if (next.maxTotalSeconds < 0.0)
        throw std::runtime_error("the maximum total length is 0 (none) or more seconds");
    if (next.gigGraceSeconds < 0.0)
        throw std::runtime_error("the grace after the gig is 0 or more seconds");
    if (next.codec != "hevc" && next.codec != "prores")
        throw std::runtime_error("unknown codec " + next.codec + " (hevc or prores)");
    if (next.target != "composition" && next.target.rfind("screen:", 0) != 0)
        throw std::runtime_error("unknown recording target " + next.target + " (composition or screen:N)");
    return next;
}

Message volumeSpace(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::space_info space = std::filesystem::space(path, error);
    if (error)
        return {{"freeMB", 0}, {"totalMB", 0}};
    return {{"freeMB", space.available >> 20}, {"totalMB", space.capacity >> 20}};
}

// Visible folders in `path`, by name.
std::vector<std::filesystem::path> subfolders(const std::filesystem::path& path)
{
    std::vector<std::filesystem::path> found;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(path, error)) {
        std::error_code kind;
        if (entry.path().filename().string().starts_with('.') || !entry.is_directory(kind))
            continue;
        found.push_back(entry.path());
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return strcasecmp(a.filename().c_str(), b.filename().c_str()) < 0;
    });
    return found;
}

} // namespace

Message listFolders(const std::filesystem::path& path, const std::filesystem::path& volumes)
{
    std::error_code error;
    const std::filesystem::path folder = std::filesystem::weakly_canonical(std::filesystem::absolute(path), error);
    if (error || !std::filesystem::is_directory(folder, error))
        throw std::runtime_error(path.string() + " is not a folder on the engine machine");
    Message reply = volumeSpace(folder);
    reply["path"] = folder.string();
    reply["parent"] = folder == folder.root_path() ? "" : folder.parent_path().string();
    reply["writable"] = ::access(folder.c_str(), W_OK) == 0;
    reply["folders"] = Message::array();
    for (const std::filesystem::path& sub : subfolders(folder))
        reply["folders"].push_back({{"name", sub.filename().string()}, {"path", sub.string()}});
    reply["volumes"] = Message::array();
    for (const std::filesystem::path& volume : subfolders(volumes)) {
        Message entry = volumeSpace(volume);
        entry["name"] = volume.filename().string();
        entry["path"] = volume.string();
        reply["volumes"].push_back(entry);
    }
    return reply;
}

Recorder::AudioSource audioSourceOf(const AudioInputStatus& input)
{
    if (input.state == "off")
        return {.reason = "the audio input is off"};
    if (input.permission == "denied" || input.permission == "restricted")
        return {.reason = "microphone access " + input.permission};
    if ((input.state != "running" && input.state != "fallback") || input.name.empty())
        return {.reason = "no audio input running"};
    std::string name = input.name;
    for (size_t i = 0; i < input.channels.size(); ++i)
        name += (i ? "+" : " ch ") + std::to_string(input.channels[i]);
    return {.name = name};
}

RecordService::RecordService(DocumentServer& document, Engine& engine, double fps, Recorder::Settings settings,
                             std::filesystem::path file)
    : document_(document), engine_(engine), file_(std::move(file)), settings_(std::move(settings)), now_(unixTime),
      recorder_(engine, fps, [this](const Recorder::Ended& e) { ended(e); },
                [this] { document_.broadcast(state("state")); })
{
    document_.handle("record", [this](const Message& message, DocumentServer::Sink reply) { handle(message, reply); });
}

RecordService::~RecordService()
{
    document_.handle("record", nullptr);
    recorder_.stop();
}

void RecordService::handle(const Message& message, const DocumentServer::Sink& reply)
{
    const std::string op = message.value("op", "");
    try {
        if (op == "state")
            return reply(state(op));
        if (op == "folders")
            return reply(folders(message));
        if (op != "set" && op != "start" && op != "stop")
            throw std::runtime_error("unknown record op " + op);
        if (op == "stop") {
            recorder_.stop();
        } else {
            apply(message);
            save();
            if (op == "start") {
                std::lock_guard lock(mutex_);
                const double now = now_();
                recorder_.start(settings_, now, gigWindowEnd(engine_.composition()->gigs, now, zone_));
            }
        }
        reply(state(op));
        Message broadcast = state("state");
        if (op == "start" && broadcast["audio"] == "unavailable")
            broadcast["notice"] = "Recording without audio: " + broadcast["audioReason"].get<std::string>();
        document_.broadcast(broadcast);
    } catch (const std::exception& e) {
        reply({{"type", "error"}, {"message", e.what()}});
    }
}

void RecordService::apply(const Message& message)
{
    std::lock_guard lock(mutex_);
    settings_ = applied(message, settings_);
}

void RecordService::save()
{
    if (file_.empty())
        return;
    try {
        std::lock_guard lock(mutex_);
        Message kept{{"codec", settings_.codec},
                     {"width", settings_.width},
                     {"height", settings_.height},
                     {"reserveMB", settings_.reserveBytes >> 20},
                     {"reservePercent", settings_.reservePercent},
                     {"maxMB", settings_.maxBytes >> 20},
                     {"maxSeconds", settings_.maxSeconds},
                     {"maxTotalSeconds", settings_.maxTotalSeconds},
                     {"gigGraceSeconds", settings_.gigGraceSeconds},
                     {"audio", settings_.audio},
                     {"precise", settings_.precise}};
        if (!settings_.path.has_extension())
            kept["path"] = settings_.path.string();
        else if (const std::optional<Message> before = readJsonFile(file_); before && before->contains("path"))
            kept["path"] = (*before)["path"];
        writeFileAtomically(file_, kept.dump(2) + "\n", false);
    } catch (const std::exception& e) {
        std::printf("record: cannot keep the settings: %s\n", e.what());
    }
}

Recorder::Settings RecordService::load(const std::filesystem::path& file, Recorder::Settings defaults)
{
    try {
        if (const std::optional<Message> kept = readJsonFile(file))
            return applied(*kept, defaults);
    } catch (const std::exception& e) {
        std::printf("record: cannot read %s (%s); the defaults apply\n", file.c_str(), e.what());
    }
    return defaults;
}

void RecordService::setWallClock(std::function<double()> now, UtcOffset zone)
{
    std::lock_guard lock(mutex_);
    now_ = std::move(now);
    zone_ = std::move(zone);
}

Recorder::Settings RecordService::settings()
{
    std::lock_guard lock(mutex_);
    return settings_;
}

void RecordService::setVolumes(std::filesystem::path volumes)
{
    std::lock_guard lock(mutex_);
    volumes_ = std::move(volumes);
}

Message RecordService::folders(const Message& message)
{
    std::filesystem::path path = message.value("path", "");
    std::filesystem::path volumes;
    {
        std::lock_guard lock(mutex_);
        volumes = volumes_;
        if (path.empty() && Recorder::folderProblem(settings_.path).empty())
            path = Recorder::movieFile(settings_.path).parent_path();
    }
    if (path.empty()) {
        const char* home = std::getenv("HOME");
        path = home ? home : "/";
    }
    Message reply = listFolders(path, volumes);
    reply["type"] = "record";
    reply["op"] = "folders";
    return reply;
}

Message RecordService::state(const std::string& op)
{
    const Recorder::Status status = recorder_.status();
    std::lock_guard lock(mutex_);
    Recorder::AudioPlan audio{status.audio, status.audioReason, status.audioSource};
    if (!status.recording)
        audio = recorder_.audioPlan(settings_.audio);
    const std::optional<double> stopsAt = status.recording ? status.stopsAt : std::nullopt;
    return {{"type", "record"},
            {"op", op},
            {"recording", status.recording},
            {"target", settings_.target},
            {"codec", settings_.codec},
            {"path", settings_.path.string()},
            {"width", settings_.width},
            {"height", settings_.height},
            {"reserveMB", settings_.reserveBytes >> 20},
            {"reservePercent", settings_.reservePercent},
            {"maxMB", settings_.maxBytes >> 20},
            {"maxSeconds", settings_.maxSeconds},
            {"maxTotalSeconds", settings_.maxTotalSeconds},
            {"gigGraceSeconds", settings_.gigGraceSeconds},
            {"stopsAt", stopsAt ? Message(*stopsAt) : Message(nullptr)},
            {"stopsAtClock", stopsAt ? clockText(localMinute(int64_t(*stopsAt), zone_)) : ""},
            {"stopsFor", stopsAt ? status.stopsFor : ""},
            {"audio", audio.state},
            {"audioReason", audio.reason},
            {"audioSource", audio.source},
            {"precise", settings_.precise},
            {"file", status.file.string()},
            {"frames", status.frames},
            {"dropped", status.dropped},
            {"hardware", status.hardware ? Message(*status.hardware) : Message(nullptr)},
            {"pixels", status.pixels},
            {"audioTrack", status.audio == "on"},
            {"audioDropped", status.audioDropped},
            {"space", spaceName(status.space)},
            {"freeMB", status.freeBytes >> 20},
            {"stopsAtMB", status.reserve >> 20},
            {"folder", Recorder::movieFile(settings_.path).parent_path().string()},
            {"folderProblem", Recorder::folderProblem(settings_.path)}};
}

void RecordService::ended(const Recorder::Ended& e)
{
    Message message{{"type", "record"}, {"op", "stopped"},   {"file", e.file.string()}, {"frames", e.frames},
                    {"dropped", e.dropped}, {"reason", e.reason}, {"kept", e.kept}};
    if (e.reason == "disk full")
        message["notice"] = "Recording stopped: the disk is full. The show goes on.";
    else if (e.reason == "max length")
        message["notice"] = "Recording finished: it reached its maximum length.";
    else if (e.reason == "gig over")
        message["notice"] = "Recording finished: the gig is over.";
    else if (e.reason != "stopped")
        message["notice"] = "Recording stopped: " + e.reason;
    std::printf("record: %s, %llu frames, %llu dropped, %s %s\n", e.reason.c_str(), (unsigned long long)e.frames,
                (unsigned long long)e.dropped, e.kept ? "kept" : "nothing kept at", e.file.c_str());
    document_.broadcast(message);
}

} // namespace azl
