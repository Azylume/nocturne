#include "audio/audio_service.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace azl {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kLogPeriod = std::chrono::milliseconds(100);

double number(const Message& message, const char* key, double low, double high)
{
    const Message& value = message[key];
    if (!value.is_number())
        throw ProtocolError(std::string(key) + " must be a number");
    const double v = value.get<double>();
    if (!(v >= low && v <= high))
        throw ProtocolError(std::string(key) + " must be within " + std::to_string(int(low)) + ".." +
                            std::to_string(int(high)));
    return v;
}

std::vector<int> channelList(const Message& message)
{
    std::vector<int> channels;
    if (!message.contains("channels"))
        return channels;
    if (!message["channels"].is_array())
        throw ProtocolError("channels must be a list of 1-based channel numbers");
    for (const Message& c : message["channels"]) {
        if (!c.is_number_integer() || c.get<int>() < 1)
            throw ProtocolError("channels must be a list of 1-based channel numbers");
        channels.push_back(c.get<int>());
    }
    return channels;
}

Message devicesMessage(const AudioSource& input)
{
    Message devices = Message::array();
    for (const AudioDeviceInfo& d : input.inputDevices())
        devices.push_back({{"uid", d.uid},
                           {"name", d.name},
                           {"channels", d.channels},
                           {"sampleRate", d.sampleRate},
                           {"default", d.isDefault}});
    return {{"type", "audio"}, {"op", "devices"}, {"devices", devices}};
}

Message appsMessage(const AudioSource& input)
{
    Message apps = Message::array();
    for (const AudioAppInfo& a : input.inputApps())
        apps.push_back({{"pid", a.pid}, {"bundle", a.bundle}, {"name", a.name}, {"playing", a.playing}});
    return {{"type", "audio"}, {"op", "apps"}, {"apps", apps}};
}

} // namespace

Message toJson(const AudioInputStatus& status)
{
    return {{"requested", status.requested},   {"channels", status.channels}, {"uid", status.uid},
            {"name", status.name},             {"sampleRate", status.sampleRate}, {"state", status.state},
            {"permission", status.permission}, {"restarts", status.restarts}};
}

Message toJson(const AudioSettings& settings)
{
    Message envelopes = Message::object();
    for (size_t i = 0; i < kAudioFeatures; ++i)
        envelopes[audioFeatureName(AudioFeature(i))] = {{"attackMs", settings.envelopes[i].attackMs},
                                                        {"releaseMs", settings.envelopes[i].releaseMs}};
    return {{"autoGain", settings.autoGain},
            {"gainDb", settings.gainDb},
            {"latencyMs", settings.latencyMs},
            {"envelopes", envelopes}};
}

AudioService::AudioService(DocumentServer& document, AudioAnalysis& analysis, AudioSource* input, Now now,
                           const std::string& logFile, const std::filesystem::path& deviceFile)
    : document_(document), analysis_(analysis), input_(input), now_(std::move(now)),
      devices_(deviceFile, analysis.settings().autoGain, analysis.settings().gainDb)
{
    if (!logFile.empty()) {
        try {
            log_ = std::make_unique<RotatingLog>(logFile);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "audio: %s\n", e.what());
        }
    }
    Message settings = toJson(analysis_.settings());
    settings["event"] = "start";
    log(settings);
    if (input_) {
        input_->onChange([this](const AudioInputStatus& status, const std::string& event) { inputChanged(status, event); });
        input_->onDevicesChange([this] { document_.broadcast(devicesMessage(*input_)); });
    }
    document_.handle("audio", [this](const Message& message, DocumentServer::Sink reply) { handle(message, reply); });
    thread_ = std::thread([this] { run(); });
}

AudioService::~AudioService()
{
    document_.handle("audio", nullptr);
    if (input_) {
        input_->onChange(nullptr);
        input_->onDevicesChange(nullptr);
    }
    {
        std::lock_guard lock(mutex_);
        running_ = false;
    }
    wake_.notify_all();
    thread_.join();
}

void AudioService::handle(const Message& message, const DocumentServer::Sink& reply)
{
    try {
        const std::string op = message.value("op", "");
        if (op == "state")
            return reply(state(now_()));
        if (op == "settings")
            return settings(message, reply);
        if (op == "subscribe") {
            const double rate = message.contains("rate") ? number(message, "rate", 1, kMaxRate) : 30.0;
            reply({{"type", "audio"}, {"op", "subscribed"}, {"rate", rate}});
            {
                std::lock_guard lock(mutex_);
                subscribers_[message["client"].get<ClientId>()] = 1.0 / rate;
            }
            wake_.notify_all();
            return;
        }
        if (op == "unsubscribe") {
            std::lock_guard lock(mutex_);
            subscribers_.erase(message["client"].get<ClientId>());
            return reply({{"type", "audio"}, {"op", "unsubscribed"}});
        }
        if (!input_)
            throw ProtocolError("this engine has no audio input");
        if (op == "devices")
            return reply(devicesMessage(*input_));
        if (op == "apps")
            return reply(appsMessage(*input_));
        if (op == "input") {
            Message status = toJson(input_->status());
            status["type"] = "audio";
            status["op"] = "input";
            return reply(status);
        }
        if (op == "select") {
            if (!message.contains("device") || !message["device"].is_string())
                throw ProtocolError("missing field device");
            const AudioChoice picked{message["device"].get<std::string>(), channelList(message)};
            devices_.pick({picked, commandLine()});
            input_->select(picked.device, picked.channels);
            return;
        }
        throw ProtocolError("unknown audio op " + op);
    } catch (const ProtocolError& e) {
        reply({{"type", "error"}, {"message", e.what()}});
    }
}

void AudioService::setCommandLine(AudioChoice choice)
{
    std::lock_guard lock(mutex_);
    commandLine_ = std::move(choice);
}

AudioChoice AudioService::commandLine()
{
    std::lock_guard lock(mutex_);
    return commandLine_;
}

void AudioService::settings(const Message& message, const DocumentServer::Sink& reply)
{
    AudioSettings s = analysis_.settings();
    if (!message.contains("autoGain") && !message.contains("gainDb") && !message.contains("latencyMs") &&
        !message.contains("envelopes")) {
        Message current = toJson(s);
        current["type"] = "audio";
        current["op"] = "settings";
        return reply(current);
    }
    if (message.contains("autoGain")) {
        if (!message["autoGain"].is_boolean())
            throw ProtocolError("autoGain must be true or false");
        s.autoGain = message["autoGain"].get<bool>();
    }
    if (message.contains("gainDb"))
        s.gainDb = number(message, "gainDb", -48, 48);
    if (message.contains("latencyMs"))
        s.latencyMs = number(message, "latencyMs", 0, AudioSettings::kMaxLatencyMs);
    if (message.contains("envelopes")) {
        if (!message["envelopes"].is_object())
            throw ProtocolError("envelopes must be an object of features");
        for (const auto& [name, envelope] : message["envelopes"].items()) {
            const auto feature = audioFeatureFromName(name);
            if (!feature)
                throw ProtocolError("unknown audio feature " + name);
            AudioEnvelope& e = s.envelopes[size_t(*feature)];
            if (envelope.contains("attackMs"))
                e.attackMs = number(envelope, "attackMs", 0, AudioSettings::kMaxEnvelopeMs);
            if (envelope.contains("releaseMs"))
                e.releaseMs = number(envelope, "releaseMs", 0, AudioSettings::kMaxEnvelopeMs);
        }
    }
    if (message.contains("autoGain") || message.contains("gainDb"))
        devices_.gainChanged(s.autoGain, s.gainDb);
    applied(s);
}

void AudioService::applied(const AudioSettings& settings)
{
    analysis_.setSettings(settings);
    Message changed = toJson(settings);
    changed["type"] = "audio";
    changed["op"] = "settings";
    document_.broadcast(changed);
    changed["event"] = "settings";
    log(changed);
}

void AudioService::inputChanged(const AudioInputStatus& status, const std::string& event)
{
    if (const auto device = devices_.inputChanged(status)) {
        AudioSettings s = analysis_.settings();
        if (s.autoGain != device->autoGain || s.gainDb != device->gainDb) {
            s.autoGain = device->autoGain;
            s.gainDb = device->gainDb;
            applied(s);
        }
        if (device->channels != status.channels)
            input_->select(status.requested, device->channels);
    }
    Message changed = toJson(status);
    changed["type"] = "audio";
    changed["op"] = "input";
    changed["event"] = event;
    document_.broadcast(changed);
    if (event.empty())
        return;
    std::printf("audio: %s\n", event.c_str());
    std::fflush(stdout);
    log(changed);
}

Message AudioService::state(double now) const
{
    const AudioFrame frame = analysis_.at(now);
    Message values = Message::object();
    for (size_t i = 0; i < kAudioFeatures; ++i)
        values[audioFeatureName(AudioFeature(i))] = std::round(frame.values[i] * 1000.0f) / 1000.0;
    Message spectrum = Message::array();
    for (float v : frame.spectrum)
        spectrum.push_back(std::round(v * 1000.0f) / 1000.0);
    Message channels = Message::array();
    for (const ChannelLevel& c : analysis_.channels())
        channels.push_back(
            {{"levelDb", std::round(c.levelDb * 10.0f) / 10.0}, {"noise", std::round(c.noise * 100.0f) / 100.0}});
    return {{"type", "audio"},
            {"op", "state"},
            {"time", frame.time},
            {"live", frame.live},
            {"silent", analysis_.digitalSilence()},
            {"overruns", analysis_.overruns()},
            {"gainDb", std::round(frame.gainDb * 10.0f) / 10.0},
            {"values", values},
            {"onsets", {{"kick", frame.onsets[0]}, {"snare", frame.onsets[1]}, {"hat", frame.onsets[2]}}},
            {"spectrum", spectrum},
            {"channels", channels}};
}

void AudioService::log(const Message& line)
{
    if (!log_)
        return;
    std::lock_guard lock(logMutex_);
    Message stamped = line;
    stamped.erase("type");
    if (!stamped.contains("time"))
        stamped["time"] = now_();
    stamped["wall"] = double(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count()) /
                      1000.0;
    log_->write(stamped.dump() + "\n");
}

void AudioService::run()
{
    std::map<ClientId, Clock::time_point> due;
    Clock::time_point nextLog = Clock::now();
    std::unique_lock lock(mutex_);
    while (running_) {
        const Clock::time_point now = Clock::now();
        std::erase_if(due, [this](const auto& d) { return !subscribers_.contains(d.first); });
        std::vector<ClientId> clients;
        for (const auto& [client, interval] : subscribers_) {
            Clock::time_point& next = due[client];
            if (now < next)
                continue;
            const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(interval));
            next = next + period > now ? next + period : now + period;
            clients.push_back(client);
        }
        const bool logDue = log_ && now >= nextLog;
        if (logDue)
            nextLog = nextLog + kLogPeriod > now ? nextLog + kLogPeriod : now + kLogPeriod;
        if (!clients.empty() || logDue) {
            lock.unlock();
            Message current = state(now_());
            std::vector<ClientId> gone;
            for (ClientId client : clients)
                if (!document_.send(client, current))
                    gone.push_back(client);
            if (logDue) {
                current.erase("spectrum");
                current.erase("op");
                log(current);
            }
            lock.lock();
            for (ClientId client : gone)
                subscribers_.erase(client);
        }
        wake_.wait_for(lock, std::chrono::milliseconds(5));
    }
}

} // namespace azl
