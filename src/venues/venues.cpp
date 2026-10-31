#include "venues/venues.hpp"

#include "platform/app_paths.hpp"
#include "protocol/messages.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

namespace azl {

namespace fs = std::filesystem;

namespace {

std::string lowercase(std::string text)
{
    for (char& c : text)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string withoutTrailingDot(std::string host)
{
    if (!host.empty() && host.back() == '.')
        host.pop_back();
    return host;
}

bool isAddress(const std::string& host)
{
    const auto numeric = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; };
    return host.find(':') != std::string::npos || std::all_of(host.begin(), host.end(), numeric);
}

// The machine's own name: "venue-mini" for venue-mini.tail1234.ts.net and venue-mini.local.
std::string shortName(const std::string& host)
{
    if (isAddress(host))
        return host;
    return lowercase(host.substr(0, host.find('.')));
}

std::string keyHelp(const fs::path& path)
{
    return "add it to the login keychain with `security add-generic-password -s " +
           std::string(kTailscaleKeychainService) + " -a " + kTailscaleKeychainAccount + " -w`, set " +
           "NOCTURNE_TAILSCALE_API_KEY, or put api_key in " + path.string() + " (chmod 600)";
}

} // namespace

fs::path defaultVenuesConfigPath()
{
    return appDataFolder() / "venues.json";
}

std::string systemTailscaleKey()
{
    if (const char* key = std::getenv("NOCTURNE_TAILSCALE_API_KEY"); key && *key)
        return key;
    const std::string command = std::string("/usr/bin/security find-generic-password -s ") +
                                kTailscaleKeychainService + " -a " + kTailscaleKeychainAccount + " -w 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe)
        return "";
    std::string key;
    char buffer[256];
    while (size_t n = fread(buffer, 1, sizeof buffer, pipe))
        key.append(buffer, n);
    if (pclose(pipe) != 0)
        return "";
    while (!key.empty() && (key.back() == '\n' || key.back() == '\r'))
        key.pop_back();
    return key;
}

TailscaleSettings loadTailscaleSettings(const fs::path& path, const std::function<std::string()>& fallbackKey)
{
    TailscaleSettings settings;
    std::error_code error;
    if (fs::exists(path, error)) {
        try {
            std::ifstream in(path);
            const Message file = Message::parse(in);
            if (!file.is_object())
                throw std::runtime_error("not a JSON object");
            settings.api = file.value("api", settings.api);
            settings.tailnet = file.value("tailnet", settings.tailnet);
            settings.tag = file.value("tag", settings.tag);
            settings.port = file.value("port", settings.port);
            if (file.contains("api_key")) {
                const auto shared = fs::perms::group_all | fs::perms::others_all;
                if ((fs::status(path).permissions() & shared) != fs::perms::none) {
                    settings.keyProblem = path.string() + " holds the Tailscale API key but others can read it: " +
                                          "chmod 600 it, or keep the key in the keychain";
                    return settings;
                }
                settings.apiKey = file.value("api_key", "");
            }
        } catch (const std::exception& e) {
            settings.keyProblem = path.string() + " cannot be read (" + e.what() + ")";
            return settings;
        }
    }
    if (settings.apiKey.empty() && fallbackKey)
        settings.apiKey = fallbackKey();
    if (settings.apiKey.empty())
        settings.keyProblem = "no Tailscale API key: " + keyHelp(path);
    return settings;
}

std::vector<Venue> parseTailscaleDevices(const std::string& json, const std::string& tag, uint16_t port)
{
    const Message reply = Message::parse(json);
    const auto devices = reply.find("devices");
    if (devices == reply.end() || !devices->is_array())
        throw std::runtime_error("no device list");
    std::vector<Venue> venues;
    for (const Message& device : *devices) {
        const std::vector<std::string> tags = device.value("tags", std::vector<std::string>{});
        if (std::find(tags.begin(), tags.end(), tag) == tags.end())
            continue;
        const std::string host = withoutTrailingDot(device.value("name", ""));
        if (host.empty())
            continue;
        const bool online = device.value("connectedToControl", device.value("online", true));
        venues.push_back({host.substr(0, host.find('.')), host, port, "tailscale", online});
    }
    std::sort(venues.begin(), venues.end(), [](const Venue& a, const Venue& b) { return a.name < b.name; });
    return venues;
}

TailscaleList fetchTailscaleVenues(const TailscaleSettings& settings, const HttpTransport& transport)
{
    if (settings.apiKey.empty())
        return {{}, settings.keyProblem.empty() ? "no Tailscale API key" : settings.keyProblem};
    HttpResponse response;
    try {
        response = transport({"GET", settings.api + "/api/v2/tailnet/" + settings.tailnet + "/devices",
                              {{"authorization", "Bearer " + settings.apiKey}, {"accept", "application/json"}}, ""});
    } catch (const std::exception& e) {
        return {{}, std::string("cannot reach the Tailscale API: ") + e.what()};
    }
    if (response.status == 401 || response.status == 403)
        return {{}, "Tailscale rejected the API key (HTTP " + std::to_string(response.status) +
                        "): make a new one in the admin console under Settings, Keys"};
    if (response.status != 200)
        return {{}, "the Tailscale API answered HTTP " + std::to_string(response.status)};
    try {
        return {parseTailscaleDevices(response.body, settings.tag, settings.port), ""};
    } catch (const std::exception& e) {
        return {{}, std::string("cannot read Tailscale's device list: ") + e.what()};
    }
}

std::vector<Venue> bonjourVenues(const std::vector<Service>& services)
{
    std::vector<Venue> venues;
    for (const Service& service : services) {
        const auto role = service.txt.find("role");
        if (role != service.txt.end() && role->second == "node")
            continue;
        venues.push_back({service.name, service.host, service.port, "bonjour", true});
    }
    return venues;
}

std::vector<Venue> mergeVenues(const std::vector<Venue>& tailscale, const std::vector<Venue>& bonjour,
                               const std::vector<Venue>& manual)
{
    std::vector<Venue> merged;
    std::set<std::string> machines;
    for (const auto* list : {&tailscale, &bonjour, &manual})
        for (const Venue& venue : *list)
            if (machines.insert(shortName(venue.host) + ":" + std::to_string(venue.port)).second)
                merged.push_back(venue);
    return merged;
}

namespace {

std::optional<double> number(const Message& object, const char* key)
{
    if (!object.is_object() || !object.contains(key) || !object[key].is_number())
        return std::nullopt;
    return object[key].get<double>();
}

VenueHealth healthOf(const Message& status)
{
    VenueHealth health;
    const Message render = status.value("render", Message::object());
    health.fps = number(render, "fps");
    health.guardLevel = render.value("guard", uint64_t(0));
    for (const Message& output : status.value("outputs", Message::array()))
        health.outputs.push_back({output.value("screenName", ""), output.value("state", "")});
    if (const Message audio = status.value("audio", Message()); audio.is_object())
        health.audioSignal = audio.value("live", false) && number(audio, "level").value_or(0.0) >= kAudioSignalFloor;
    if (const std::optional<double> missing = number(status.value("media", Message()), "missing"))
        health.missingMedia = int(*missing);
    return health;
}

} // namespace

VenueProbe probeVenue(const std::string& host, uint16_t port, std::chrono::milliseconds timeout,
                      const std::string& token)
{
    struct State {
        std::mutex mutex;
        std::condition_variable arrived;
        std::optional<Message> snapshot, status;
        std::string error;
        bool closed = false;
    };
    auto state = std::make_shared<State>();
    VenueProbe probe;
    std::unique_ptr<WebSocketClient> socket;
    try {
        socket = std::make_unique<WebSocketClient>(
            host, port, Encoding::MessagePack,
            [state](const Message& message) {
                const std::string type = message.value("type", "");
                std::lock_guard lock(state->mutex);
                if (type == "snapshot")
                    state->snapshot = message;
                else if (type == "machine" && message.value("op", "") == "status")
                    state->status = message;
                else if (type == "error" && state->error.empty())
                    state->error = message.value("message", "error");
                state->arrived.notify_all();
            },
            [state] {
                std::lock_guard lock(state->mutex);
                state->closed = true;
                state->arrived.notify_all();
            });
    } catch (const std::exception& e) {
        probe.error = e.what();
        return probe;
    }
    probe.reachable = true;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    Message hello = msg::hello(token);
    hello["watch"] = false;
    socket->send(hello);
    socket->send({{"type", "machine"}, {"op", "status"}});
    std::unique_lock lock(state->mutex);
    state->arrived.wait_until(lock, deadline, [&] { return state->status || !state->error.empty() || state->closed; });
    if (state->status) {
        probe.answered = true;
        probe.build = state->status->value("version", "");
        probe.health = healthOf(*state->status);
    } else if (state->snapshot) {
        probe.answered = true;
        probe.build = state->snapshot->value("build", "");
    } else {
        probe.engineError = state->error;
        probe.error = state->closed ? "the engine closed the connection" : "no answer from the engine";
    }
    return probe;
}

const char* dotName(VenueDot dot)
{
    switch (dot) {
    case VenueDot::Ok:
        return "ok";
    case VenueDot::Warning:
        return "warning";
    case VenueDot::Fault:
        return "fault";
    case VenueDot::Offline:
        return "offline";
    }
    return "offline";
}

namespace {

std::string whole(double value)
{
    return std::to_string(int64_t(std::lround(value)));
}

void outputAlerts(const std::vector<OutputHealth>& outputs, std::vector<VenueAlert>& alerts)
{
    for (const OutputHealth& output : outputs) {
        if (output.state == "missing")
            alerts.push_back({AlertKind::OutputMissing, VenueDot::Fault, output.name + " missing"});
        else if (output.state == "mirrored")
            alerts.push_back({AlertKind::OutputMirrored, VenueDot::Warning, output.name + " mirrored"});
        else if (output.state == "low rate")
            alerts.push_back({AlertKind::OutputLowRate, VenueDot::Warning, output.name + " low rate"});
    }
}

void healthAlerts(const VenueHealth& health, std::vector<VenueAlert>& alerts)
{
    if (health.fps && *health.fps < kLowFps)
        alerts.push_back({AlertKind::LowFps, *health.fps < kFpsFloor ? VenueDot::Fault : VenueDot::Warning,
                          whole(*health.fps) + " fps"});
    if (health.guardLevel > kGuardWarning)
        alerts.push_back(
            {AlertKind::FrameGuard, VenueDot::Warning, "frame guard " + std::to_string(health.guardLevel)});
    if (health.audioSignal == false && health.silentSeconds >= kNoAudioSeconds)
        alerts.push_back({AlertKind::NoAudio, VenueDot::Fault, "no audio " + whole(health.silentSeconds) + " s"});
    if (health.missingMedia.value_or(0) > 0)
        alerts.push_back(
            {AlertKind::MissingMedia, VenueDot::Warning, std::to_string(*health.missingMedia) + " media missing"});
    outputAlerts(health.outputs, alerts);
}

} // namespace

std::vector<VenueAlert> venueAlerts(const Venue& venue, const VenueProbe& probe, const AlertContext& context)
{
    const VenueDot offline = context.inGigWindow ? VenueDot::Fault : VenueDot::Offline;
    if (!venue.online || !probe.reachable || (!probe.answered && probe.engineError.empty()))
        return {{AlertKind::Offline, offline, "offline"}};
    if (!probe.answered)
        return {{AlertKind::EngineError, VenueDot::Fault, probe.engineError}};
    std::vector<VenueAlert> alerts;
    if (probe.health)
        healthAlerts(*probe.health, alerts);
    if (!context.uiBuild.empty())
        if (std::string warning = versionWarning(probe.build, context.uiBuild); !warning.empty())
            alerts.push_back({AlertKind::VersionMismatch, VenueDot::Warning, warning});
    return alerts;
}

VenueDot venueDot(const std::vector<VenueAlert>& alerts)
{
    VenueDot worst = VenueDot::Ok;
    for (const VenueAlert& alert : alerts)
        worst = std::max(worst, alert.dot);
    return worst;
}

VenueDot venueDot(const Venue& venue, const VenueProbe& probe, const AlertContext& context)
{
    return venueDot(venueAlerts(venue, probe, context));
}

std::string versionWarning(const std::string& venueBuild, const std::string& ours)
{
    if (venueBuild.empty() || venueBuild == ours)
        return "";
    return "venue build " + venueBuild + ", UI build " + ours;
}

} // namespace azl
