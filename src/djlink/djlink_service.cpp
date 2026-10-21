#include "djlink/djlink_service.hpp"

#include "document/composition_file.hpp"

#include <algorithm>
#include <cstdio>

namespace azl::djlink {

namespace {

const char* kindName(DeviceKind kind)
{
    switch (kind) {
    case DeviceKind::Player:
        return "player";
    case DeviceKind::Mixer:
        return "mixer";
    case DeviceKind::Computer:
        return "computer";
    }
    return "computer";
}

std::string ipText(const std::array<uint8_t, 4>& ip)
{
    return std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." +
           std::to_string(ip[3]);
}

std::string macText(const std::array<uint8_t, 6>& mac)
{
    char text[18];
    std::snprintf(text, sizeof text, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return text;
}

template <typename T>
Message orNull(const std::optional<T>& value)
{
    return value ? Message(*value) : Message();
}

// The status packet's play state, else "playing" while beats arrive (players beat only while playing).
std::string playText(const LinkState::Player& p, bool fresh, bool live)
{
    if (!fresh)
        return live ? "playing" : "";
    switch (p.status->playState) {
    case 0:
        return "no track";
    case 2:
        return "loading";
    case 3:
        return "playing";
    case 4:
        return "looping";
    case 5:
        return "paused";
    case 6:
        return "cued";
    case 9:
        return "searching";
    case 0x11:
        return "ended";
    }
    return p.status->playing ? "playing" : "stopped";
}

Message playerJson(const LinkState& state, int number, const LinkState::Player& p, double now)
{
    const bool live = state.live(p, now);
    const bool fresh = p.status && now - p.statusTime < LinkState::kStatusTimeout;
    Message track;
    if (fresh && p.status->trackId)
        track = {{"player", p.status->trackPlayer}, {"slot", p.status->trackSlot}, {"type", p.status->trackType},
                 {"id", p.status->trackId}};
    return {
        {"number", number},
        {"name", p.name},
        {"live", live},
        {"grid", state.grid(p, now)},
        {"onAir", orNull(state.onAirFlag(p, now))},
        {"master", state.master(p, now)},
        {"bpm", p.beat ? Message(state.bpm(p)) : Message()},
        {"trackBpm", p.beat ? Message(p.beat->trackBpm) : Message()},
        {"pitch", p.beat ? Message(pitchPercent(p.beat->pitch)) : Message()},
        {"beatInBar", live ? Message(p.beat->beatInBar) : Message()},
        {"playing", fresh ? Message(p.status->playing) : Message()},
        {"playState", fresh ? Message(p.status->playState) : Message()},
        {"play", playText(p, fresh, live)},
        {"track", track},
    };
}

bool heard(const LinkState& state, const LinkState::Player& p, double now)
{
    return state.live(p, now) || (p.status && now - p.statusTime < LinkState::kStatusTimeout);
}

} // namespace

Settings loadSettings(const std::filesystem::path& file)
{
    try {
        const std::optional<nlohmann::json> j = readJsonFile(file);
        if (!j)
            return {};
        return {j->value("enabled", false), j->value("interface", "")};
    } catch (const std::exception& e) {
        std::printf("djlink: %s; starting off\n", e.what());
        return {};
    }
}

void saveSettings(const std::filesystem::path& file, const Settings& settings)
{
    writeFileAtomically(file, nlohmann::json{{"enabled", settings.enabled}, {"interface", settings.interface}}.dump(2));
}

Service::Service(DocumentServer& document, Now now, std::filesystem::path settingsFile, Receiver::Options receiver)
    : document_(document), now_(std::move(now)), file_(std::move(settingsFile)), options_(std::move(receiver))
{
    if (!file_.empty())
        apply(loadSettings(file_));
    document_.handle("djlink", [this](const Message& message, DocumentServer::Sink reply) { handle(message, reply); });
}

Service::~Service()
{
    document_.handle("djlink", nullptr);
    std::lock_guard lock(settingsMutex_);
    receiver_.reset();
}

Settings Service::settings() const
{
    std::lock_guard lock(settingsMutex_);
    return settings_;
}

std::optional<OnAirBeat> Service::onAirBeat(double now)
{
    std::lock_guard lock(stateMutex_);
    return state_.onAirBeat(now);
}

void Service::apply(const Settings& settings)
{
    std::lock_guard lock(settingsMutex_);
    settings_ = settings;
    receiver_.reset();
    {
        std::lock_guard state(stateMutex_);
        state_.clear();
    }
    if (!settings.enabled || settings.interface.empty())
        return;
    Receiver::Options options = options_;
    options.interface = settings.interface;
    receiver_ = std::make_unique<Receiver>(
        options, [this](uint16_t port, const uint8_t* data, size_t size) { datagram(port, data, size); });
}

void Service::datagram(uint16_t port, const uint8_t* data, size_t size)
{
    const std::optional<Packet> packet = parse(port, data, size);
    const double now = now_();
    std::lock_guard lock(stateMutex_);
    if (packet)
        state_.receive(*packet, now);
    else
        ++unknown_;
}

Message Service::status()
{
    const double now = now_();
    Message m{{"type", "djlink"}, {"op", "status"}, {"time", now}};
    {
        std::lock_guard lock(settingsMutex_);
        m["enabled"] = settings_.enabled;
        m["interface"] = settings_.interface;
        m["listening"] = receiver_ && receiver_->listening();
        m["error"] = receiver_ ? receiver_->error() : settings_.enabled ? "no interface set" : "";
        m["datagrams"] = receiver_ ? receiver_->datagrams() : 0;
    }
    std::lock_guard lock(stateMutex_);
    m["unknown"] = unknown_;
    const std::optional<OnAirBeat> beat = state_.onAirBeat(now);
    m["onAir"] = beat ? beat->player : 0;
    m["bpm"] = beat ? Message(beat->bpm) : Message();
    m["beat"] = beat ? Message{{"inBar", beat->beatInBar}, {"phase", std::clamp(beat->beatsSince(now), 0.0, 1.0)}}
                     : Message();
    Message devices = Message::array();
    for (const auto& [number, d] : state_.devices())
        if (now - d.seen < LinkState::kDeviceTimeout)
            devices.push_back({{"number", number}, {"name", d.announcement.name}, {"kind", kindName(kindOf(number))},
                               {"ip", ipText(d.announcement.ip)}, {"mac", macText(d.announcement.mac)},
                               {"seen", now - d.seen}});
    m["devices"] = devices;
    Message players = Message::array();
    for (const auto& [number, p] : state_.players())
        if (heard(state_, p, now))
            players.push_back(playerJson(state_, number, p, now));
    m["players"] = players;
    const std::optional<MixerStatus> mixer = state_.mixer(now);
    m["mixer"] = mixer ? Message{{"number", mixer->number}, {"name", mixer->name}, {"bpm", mixer->bpm},
                                 {"beatInBar", mixer->beatInBar}, {"master", mixer->master}}
                       : Message();
    return m;
}

Message Service::summary()
{
    const Message s = status();
    Message players = Message::array();
    for (const Message& p : s["players"])
        players.push_back({{"number", p["number"]}, {"onAir", p["onAir"]}, {"bpm", p["bpm"]}, {"pitch", p["pitch"]},
                           {"play", p["play"]}, {"beatInBar", p["beatInBar"]}});
    return {{"enabled", s["enabled"]},
            {"interface", s["interface"]},
            {"listening", s["listening"]},
            {"error", s["error"]},
            {"datagrams", s["datagrams"]},
            {"onAir", s["onAir"]},
            {"bpm", s["bpm"]},
            {"beatInBar", s["beat"].is_object() ? s["beat"]["inBar"] : Message()},
            {"players", players}};
}

std::string statusLine(const Message& summary)
{
    if (!summary.value("enabled", false))
        return "djlink off";
    char text[160];
    std::string line = "djlink " + summary.value("interface", std::string());
    line += summary.value("listening", false) ? ", listening"
                                              : ", not listening: " + summary.value("error", std::string());
    line += ", " + std::to_string(summary.value("datagrams", uint64_t(0))) + " datagrams";
    if (const int onAir = summary.value("onAir", 0)) {
        std::snprintf(text, sizeof text, ", on air %d at %.2f BPM", onAir, summary["bpm"].get<double>());
        line += text;
    }
    if (summary.value("following", false))
        line += ", following";
    for (const Message& p : summary.value("players", Message::array())) {
        line += " | player " + std::to_string(p["number"].get<int>());
        if (p["bpm"].is_number()) {
            std::snprintf(text, sizeof text, " %.2f BPM %+.2f%%", p["bpm"].get<double>(), p["pitch"].get<double>());
            line += text;
        }
        if (const std::string play = p["play"]; !play.empty())
            line += " " + play;
        if (p["beatInBar"].is_number())
            line += " beat " + std::to_string(p["beatInBar"].get<int>());
        if (p["onAir"].is_boolean())
            line += p["onAir"].get<bool>() ? " on air" : " off air";
    }
    return line;
}

void Service::handle(const Message& message, const DocumentServer::Sink& reply)
{
    try {
        const std::string op = message.value("op", "");
        if (op == "status")
            return reply(status());
        if (op != "set")
            throw ProtocolError("unknown djlink op " + op);
        if (!message.contains("enabled") || !message["enabled"].is_boolean())
            throw ProtocolError("enabled must be true or false");
        Settings next = settings();
        next.enabled = message["enabled"].get<bool>();
        if (message.contains("interface")) {
            if (!message["interface"].is_string())
                throw ProtocolError("interface must be a name such as en7");
            next.interface = message["interface"].get<std::string>();
        }
        if (next.enabled && next.interface.empty())
            throw ProtocolError("djlink needs a network interface, e.g. en7");
        if (!file_.empty())
            saveSettings(file_, next);
        apply(next);
        document_.broadcast(status());
    } catch (const std::exception& e) {
        reply({{"type", "error"}, {"message", e.what()}});
    }
}

} // namespace azl::djlink
