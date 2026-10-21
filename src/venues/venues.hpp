#pragma once

#include "net/websocket.hpp"
#include "sync/discovery.hpp"
#include "upload/http.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace azl {

inline constexpr const char* kVenueTag = "tag:nocturne-venue";
inline constexpr const char* kTailscaleKeychainService = "nocturne-tailscale";
inline constexpr const char* kTailscaleKeychainAccount = "api-key";

// A venue machine the Director can connect to.
struct Venue {
    std::string name;
    std::string host;
    uint16_t port = kDefaultPort;
    // "tailscale", "bonjour" or "manual".
    std::string source;
    // Tailscale's own view of the device; Bonjour and typed entries count as online.
    bool online = true;

    bool operator==(const Venue&) const = default;
};

// Where the Director finds its venues on the tailnet. The API key stays on the Director: in venues.json only when
// nobody else can read the file, else in NOCTURNE_TAILSCALE_API_KEY or the login keychain.
struct TailscaleSettings {
    std::string api = "https://api.tailscale.com";
    // "-" is the key's own tailnet.
    std::string tailnet = "-";
    std::string tag = kVenueTag;
    uint16_t port = kDefaultPort;
    std::string apiKey;
    // Why there is no key, for the user; empty when there is one.
    std::string keyProblem;
};

// ~/Library/Application Support/Nocturne/venues.json
std::filesystem::path defaultVenuesConfigPath();
// NOCTURNE_TAILSCALE_API_KEY, else the login keychain (service nocturne-tailscale, account api-key); empty when
// neither.
std::string systemTailscaleKey();
// The file's tailnet, tag, port and api_key, then `fallbackKey` for the key. Never throws: what is wrong with the file
// or the key lands in keyProblem.
TailscaleSettings loadTailscaleSettings(const std::filesystem::path& path = defaultVenuesConfigPath(),
                                        const std::function<std::string()>& fallbackKey = systemTailscaleKey);

// The devices carrying `tag` in a Tailscale API v2 device list, by their MagicDNS name. Throws when the text is not
// such a list.
std::vector<Venue> parseTailscaleDevices(const std::string& json, const std::string& tag, uint16_t port);

struct TailscaleList {
    std::vector<Venue> venues;
    // Plain words for the user: no key, a rejected key, no network; empty when the list came.
    std::string error;
};

using HttpTransport = std::function<HttpResponse(const HttpRequest&)>;
TailscaleList fetchTailscaleVenues(const TailscaleSettings& settings, const HttpTransport& transport = httpRequest);

// The engines Bonjour found, leaving out Node Sync followers.
std::vector<Venue> bonjourVenues(const std::vector<Service>& services);
// Tailscale's, then Bonjour's and the typed ones of other engines: one entry per engine, by its machine's short host
// name and its port.
std::vector<Venue> mergeVenues(const std::vector<Venue>& tailscale, const std::vector<Venue>& bonjour,
                               const std::vector<Venue>& manual);

// One output of a venue machine as its machine status names it.
struct OutputHealth {
    std::string name;
    // "ok", "low rate", "missing" or "mirrored".
    std::string state;

    bool operator==(const OutputHealth&) const = default;
};

// A venue machine's health: the machine status fields the dot rules read; a figure it lacked is empty.
struct VenueHealth {
    std::optional<double> fps;
    // Layers the frame budget guard draws at half resolution.
    uint64_t guardLevel = 0;
    std::vector<OutputHealth> outputs;
    // Empty while the engine runs no audio analysis.
    std::optional<bool> audioSignal;
    // How long the Director has seen no audio signal in a row.
    double silentSeconds = 0.0;
    // Media the composition needs and the machine lacks (the status's "media": {"missing"}); empty without it.
    std::optional<int> missingMedia;

    bool operator==(const VenueHealth&) const = default;
};

// What a short look at a venue's engine found.
struct VenueProbe {
    bool reachable = false;
    // The engine answered with its machine status, or an engine older than the status with its snapshot.
    bool answered = false;
    std::string build;
    std::optional<VenueHealth> health;
    // Why there was no answer: the connection failed, closed or stayed silent.
    std::string error;
    // What the engine answered instead.
    std::string engineError;
};

// Connects, says hello without watching the document (with the venue machine's pairing `token`), asks the machine
// status and leaves; the engine changes nothing for it and sends no document.
VenueProbe probeVenue(const std::string& host, uint16_t port,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(4000), const std::string& token = {});

enum class VenueDot { Ok, Warning, Fault, Offline };
const char* dotName(VenueDot dot);

enum class AlertKind {
    Offline,
    EngineError,
    LowFps,
    FrameGuard,
    NoAudio,
    MissingMedia,
    OutputMissing,
    OutputMirrored,
    OutputLowRate,
    VersionMismatch
};

struct VenueAlert {
    AlertKind kind = AlertKind::Offline;
    VenueDot dot = VenueDot::Offline;
    // "Screen 2 missing", "no audio 12 s", "24 fps"
    std::string text;

    bool operator==(const VenueAlert&) const = default;
};

// The thresholds of the dot rules.
inline constexpr double kFpsFloor = 30.0;
inline constexpr double kLowFps = 50.0;
inline constexpr uint64_t kGuardWarning = 1;
inline constexpr double kNoAudioSeconds = 10.0;
// The audio level at or above which the input carries a signal.
inline constexpr double kAudioSignalFloor = 0.01;

struct AlertContext {
    // Inside one of the venue's Gig windows offline is a fault.
    bool inGigWindow = false;
    // Empty skips the version check.
    std::string uiBuild;
};

// The dot rules, for the venue menu and the alert strip alike:
//   offline: Tailscale says so or no engine answers; a fault inside a Gig window
//   fault: the engine answered with an error, an output missing, no audio signal for kNoAudioSeconds, render fps
//     below kFpsFloor
//   warning: render fps below kLowFps, an output at a low rate or mirrored, the frame guard above kGuardWarning,
//     media missing, another build than the UI's
std::vector<VenueAlert> venueAlerts(const Venue& venue, const VenueProbe& probe, const AlertContext& context = {});
// The worst alert's dot; ok without any.
VenueDot venueDot(const std::vector<VenueAlert>& alerts);
VenueDot venueDot(const Venue& venue, const VenueProbe& probe, const AlertContext& context = {});
// "venue build A, UI build B" when they differ; empty when they match or the venue's is unknown.
std::string versionWarning(const std::string& venueBuild, const std::string& ours);

} // namespace azl
