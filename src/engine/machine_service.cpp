#include "engine/machine_service.hpp"

#include "document/composition_file.hpp"
#include "engine/cue_service.hpp"
#include "protocol/build_info.hpp"

#include <algorithm>
#include <cmath>

namespace azl {

namespace {

constexpr double kBar = 4.0;

template <typename T>
Message orNull(const std::optional<T>& value)
{
    return value ? Message(*value) : Message(nullptr);
}

Message figure(const Message& object, const char* key)
{
    return object.is_object() && object.contains(key) && object[key].is_number() ? object[key] : Message(nullptr);
}

const Message* outputFigures(const Message& stats, const std::string& display)
{
    if (!stats.is_object() || !stats.contains("outputs") || !stats["outputs"].is_array())
        return nullptr;
    for (const Message& output : stats["outputs"])
        if (output.value("display", "") == display)
            return &output;
    return nullptr;
}

std::string outputState(const DisplayInfo* display, const Message& presents)
{
    if (!display)
        return "missing";
    if (!display->mirrorOf.empty())
        return "mirrored";
    if (presents.is_number() && presents.get<double>() < MachineService::kLowRate * display->refreshHz)
        return "low rate";
    return "ok";
}

Message thermalJson(const Thermal& thermal)
{
    return {{"cpuSpeedLimit", orNull(thermal.cpuSpeedLimit)},
            {"temperatureC", orNull(thermal.temperatureC)},
            {"state", thermal.state.empty() ? Message(nullptr) : Message(thermal.state)}};
}

} // namespace

MachineService::MachineService(DocumentServer& document, Engine& engine, Displays& displays, const KnownDisplays* known,
                               Now now, Post post, MachineSources sources)
    : document_(document), engine_(engine), displays_(displays), known_(known), now_(std::move(now)),
      post_(std::move(post)), sources_(std::move(sources)), actions_(sources_.showLock, sources_.restart, sources_.stop),
      update_(sources_.update)
{
    document_.handle("machine", [this](const Message& message, DocumentServer::Sink reply) { handle(message, reply); });
    thread_ = std::thread([this] { run(); });
}

MachineService::~MachineService()
{
    document_.handle("machine", nullptr);
    {
        std::lock_guard lock(mutex_);
        running_ = false;
    }
    wake_.notify_all();
    thread_.join();
}

size_t MachineService::subscribers()
{
    std::lock_guard lock(mutex_);
    return subscribers_.size();
}

void MachineService::handle(const Message& message, const DocumentServer::Sink& reply)
{
    const std::string op = message.value("op", "");
    const ClientId client = message.value("client", ClientId(0));
    if (op == "status") {
        const Thermal now = thermal();
        return post_([this, reply, now] { reply(status(now)); });
    }
    if (op == "subscribe") {
        const Message rate = message.value("rate", Message(2.0));
        if (!rate.is_number() || rate.get<double>() < kMinRate || rate.get<double>() > kMaxRate)
            return reply({{"type", "error"},
                          {"message", "rate must be within " + std::to_string(kMinRate) + ".." +
                                          std::to_string(kMaxRate) + " Hz"}});
        {
            std::lock_guard lock(mutex_);
            subscribers_[client] = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(1.0 / rate.get<double>()));
        }
        wake_.notify_all();
        return;
    }
    if (op == "unsubscribe") {
        std::lock_guard lock(mutex_);
        subscribers_.erase(client);
        return;
    }
    if (op == "testcard" || op == "identify" || op == "unmirror")
        return act(op, message, reply);
    if (op == "update")
        return reply(update_.receive(message, [this] { return updateRefusal(); }));
    if (actions_.handle(message, reply))
        return;
    reply({{"type", "error"}, {"message", "unknown machine op " + op}});
}

void MachineService::act(const std::string& op, const Message& message, const DocumentServer::Sink& reply)
{
    if (op == "testcard") {
        if (!message.value("on", Message()).is_boolean())
            return reply({{"type", "error"}, {"message", "testcard needs \"on\": true or false"}});
        const bool on = message["on"].get<bool>();
        try {
            document_.perform(SetScreenTestCard{0, on}, "machine", message.value("client", ClientId(0)));
        } catch (const std::exception& e) {
            return reply({{"type", "error"}, {"message", e.what()}});
        }
        return reply({{"type", "machine"}, {"op", "testcard"}, {"on", on}});
    }
    if (op == "identify") {
        const Message seconds = message.value("seconds", Message(3.0));
        if (!seconds.is_number() || seconds.get<double>() < 0.0)
            return reply({{"type", "error"}, {"message", "identify needs seconds of 0 or more"}});
        return post_([this, reply, seconds = seconds.get<double>()] {
            if (sources_.identify)
                sources_.identify(seconds);
            reply({{"type", "machine"}, {"op", "identify"}, {"seconds", seconds}});
        });
    }
    post_([this, reply] {
        reply({{"type", "machine"}, {"op", "unmirror"}, {"ended", sources_.unmirror ? sources_.unmirror() : 0}});
    });
}

void MachineService::run()
{
    std::map<ClientId, Clock::time_point> due;
    std::unique_lock lock(mutex_);
    while (running_) {
        const Clock::time_point now = Clock::now();
        std::erase_if(due, [this](const auto& d) { return !subscribers_.contains(d.first); });
        std::vector<ClientId> clients;
        Clock::time_point wakeAt = Clock::time_point::max();
        for (const auto& [client, period] : subscribers_) {
            Clock::time_point& next = due[client];
            if (now >= next) {
                next = next + period > now ? next + period : now + period;
                clients.push_back(client);
            }
            wakeAt = std::min(wakeAt, next);
        }
        if (!clients.empty()) {
            lock.unlock();
            send(std::move(clients));
            lock.lock();
            continue;
        }
        if (wakeAt == Clock::time_point::max())
            wake_.wait(lock);
        else
            wake_.wait_until(lock, wakeAt);
    }
}

void MachineService::send(std::vector<ClientId> clients)
{
    const Thermal now = thermal();
    post_([this, clients = std::move(clients), now] {
        const Message current = status(now);
        std::vector<ClientId> gone;
        for (ClientId client : clients)
            if (!document_.send(client, current))
                gone.push_back(client);
        std::lock_guard lock(mutex_);
        for (ClientId client : gone)
            subscribers_.erase(client);
    });
}

Thermal MachineService::thermal()
{
    std::lock_guard lock(thermalMutex_);
    const Clock::time_point now = Clock::now();
    if (!thermalAt_ || now - *thermalAt_ >= kThermalEvery) {
        thermal_ = sources_.thermal ? sources_.thermal() : Thermal{};
        thermalAt_ = now;
    }
    return thermal_;
}

Message MachineService::status(const Thermal& thermal) const
{
    const std::shared_ptr<const Composition> composition = engine_.composition();
    const TransportState transport = engine_.transport();
    const double beat = beatAt(engine_, now_());
    const Message stats = sources_.stats ? sources_.stats() : Message();
    const Message render = stats.is_object() ? stats.value("render", Message()) : Message();
    const double inBar = std::fmod(std::max(beat, 0.0), kBar);
    return {{"type", "machine"},
            {"op", "status"},
            {"mode", "normal"},
            {"showLock", actions_.locked()},
            {"testCard", composition->testCard},
            {"version", buildId()},
            {"render", {{"fps", figure(render, "fps")}, {"gpuMs", figure(render, "gpuMs")}, {"guard", transport.budgetLevel}}},
            {"thermal", thermalJson(thermal)},
            {"nextCue", nextCue(*composition, beat, transport.tempo)},
            {"outputs", outputs(*composition, stats)},
            {"audio", audio()},
            {"clock",
             {{"bpm", transport.bpm},
              {"beat", beat},
              {"bar", int64_t(std::floor(std::max(beat, 0.0) / kBar)) + 1},
              {"beatInBar", int(inBar) + 1},
              {"djlink", sources_.djlink ? sources_.djlink() : Message(nullptr)}}},
            {"update", {{"refused", orNull(updateRefusal())}, {"last", update_.lastResult()}}},
            {"media", sources_.media ? sources_.media() : Message(nullptr)}};
}

std::optional<std::string> MachineService::updateRefusal() const
{
    if (!update_.served())
        return "Update needs the supervisor and an installed bundle, and this engine runs without them";
    if (update_.refused())
        return update_.refused();
    if (std::optional<std::string> locked = actions_.refusal("Update"))
        return locked;
    const double now = sources_.wall ? sources_.wall() : unixTime();
    for (const GigWindow& window : engine_.composition()->gigs)
        if (inGigWindow({window}, now, "update", sources_.zone))
            return "the Gig window " + window.date + " " + clockText(window.start) + "-" + clockText(window.end) +
                   " is on: Update is refused until it ends";
    return std::nullopt;
}

Message MachineService::outputs(const Composition& composition, const Message& stats) const
{
    const std::vector<DisplayInfo> attached = displays_.attached();
    Message list = Message::array();
    for (const Screen& screen : composition.screens) {
        if (screen.display.empty())
            continue;
        const auto found = std::find_if(attached.begin(), attached.end(),
                                        [&](const DisplayInfo& d) { return d.id == screen.display; });
        const DisplayInfo* display = found == attached.end() ? nullptr : &*found;
        std::string name = display ? display->name : "";
        if (name.empty() && known_)
            if (const std::optional<DisplayInfo> known = known_->find(screen.display))
                name = known->name;
        const Message* figures = display ? outputFigures(stats, screen.display) : nullptr;
        const Message presents = figures ? figure(*figures, "presents") : Message(nullptr);
        list.push_back({{"screen", screen.id},
                        {"screenName", screen.name},
                        {"display", screen.display},
                        {"displayName", name.empty() ? screen.display : name},
                        {"hz", display ? Message(display->refreshHz) : Message(nullptr)},
                        {"presents", presents},
                        {"hitches", figures ? figure(*figures, "hitches") : Message(nullptr)},
                        {"state", outputState(display, presents)}});
    }
    return list;
}

Message MachineService::nextCue(const Composition& composition, double beat, double tempo) const
{
    const std::vector<CueFiring> upcoming = upcomingCues(composition, beat);
    if (upcoming.empty())
        return nullptr;
    const CueFiring& next = upcoming.front();
    const auto cue = std::find_if(composition.cues.begin(), composition.cues.end(),
                                  [&](const Cue& c) { return c.id == next.cue; });
    const std::string name =
        cue != composition.cues.end() && !cue->name.empty() ? cue->name : "Cue " + std::to_string(next.cue);
    return {{"cue", next.cue},
            {"name", name},
            {"beat", next.beat},
            {"bar", int64_t(std::floor(next.beat / kBar)) + 1},
            {"seconds", tempo > 0.0 ? (next.beat - beat) * 60.0 / tempo : 0.0}};
}

Message MachineService::audio() const
{
    const AudioAnalysis* analysis = audio_.load();
    if (!analysis)
        return nullptr;
    const AudioFrame frame = analysis->latest();
    Message audio{{"device", nullptr},
                  {"state", nullptr},
                  {"channels", Message::array()},
                  {"gainDb", frame.gainDb},
                  {"level", frame.values[size_t(AudioFeature::Level)]},
                  {"live", frame.live}};
    if (sources_.input) {
        const AudioInputStatus input = sources_.input->status();
        audio["device"] = input.name.empty() ? Message(nullptr) : Message(input.name);
        audio["state"] = input.state;
        audio["channels"] = input.channels;
    }
    return audio;
}

} // namespace azl
