#include "app/engine_config.hpp"

#include "effects/preset_service.hpp"
#include "engine/engine.hpp"
#include "engine/modulation_service.hpp"
#include "media/media_library.hpp"
#include "net/websocket.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace azl::app {

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(EngineConfig, display, main, windowed, virtualDisplays, offscreen,
                                                listDisplays, slowDisplay, slowMs, seconds, fps, width, height, port,
                                                library, profile, clipPreviews, seed, presets,
                                                macroSets, statsJson, frameTrace, realtimeFrames, debugFrameBits,
                                                opaqueOutputs, gpuProfile, frameGuard, fsrUpscale, audioInput,
                                                audioChannels, audioLog, audioGain, audioLatencyMs, micCheck, state, composition,
                                                mediaRoot, safeMode, fallbackImage, node, follow, name,
                                                recordPath)

EngineConfig::EngineConfig()
    : port(kDefaultPort), library(MediaLibrary::defaultRoot().string()), seed(std::random_device{}()), presets(PresetService::defaultFile().string()),
      macroSets(ModulationService::defaultFile().string())
{
}

const char* const kEngineUsage = R"(usage: nocturne [options]
  --display ID    display of the starting screen, a full slice of the composition (default: the first display)
  --main ID       display whose output sets the render clock's phase while attached (default: the first screen's)
  --displays      list the attached displays with their ids and quit
  --windowed      regular windows instead of borderless fullscreen
  --virtual N     N simulated displays window-1..window-N, windows side by side on the main display
  --offscreen     no windows and no app activation: the --virtual displays (default 1) render into textures and
                  present to nothing at 60 Hz; stats still count
  --slow ID:MS    hold every present of that display's output back by MS milliseconds
  --seconds N     quit after N seconds
  --fps N         render rate (default 60)
  --size WxH      composition size (default 1920x1080)
  --port N        protocol WebSocket port on every interface (default 7400). Clients on other machines must present
                  this machine's pairing token, kept in the --state folder (default ~/Library/Application
                  Support/Nocturne/State) as "token" and shown by nocturne-ctl token; clients on this machine need none
  --loopback      bind the protocol port to 127.0.0.1 alone, so nothing on the LAN can reach it (the preview app sets
                  this; remote control then goes through an ssh or Tailscale tunnel)
  --library DIR   media library index folder (default ~/Library/Application Support/Nocturne/MediaLibrary)
  --clip-cache MB bound on the two GOPs a clip playing backward holds (default 2048)
  --preload MB    bound on the frames of clips preloaded for the next trigger (default 3072)
  --kept-previews N
                  clip previews kept for a selection back besides the watched ones (default 8)
  --profile smooth|sharp
                  smooth: the frame guard lowers resolution as the GPU time nears the frame budget; sharp: only once
                  frames overrun it (default smooth)
  --protect-transitions on|off
                  during a transition the frame guard lowers resolution before a frame would drop (default off)
  --bake-static on|off
                  a chain's leading passes whose inputs stay the same (no modulation, no clock, a still picture) are
                  drawn once and reused, with the same picture (default off)
  --seed N        Random Jump and random Autopilot seed (default random)
  --clip-previews start with a moving preview of the clip a UI has selected and is not playing, with the clip's own
                  effects (off by default: on an engine that drives outputs it takes keyframe decodes and small GPU
                  draws besides the show's; while off a UI shows its still). A UI or `nocturne-ctl clippreviews on|off`
                  turns them on and off live
  --presets FILE  effect presets file (default ~/Library/Application Support/Nocturne/Presets.json)
  --macro-sets FILE
                  Macro Sets file (default ~/Library/Application Support/Nocturne/MacroSets.json)
  --stats-json F  also append the stats as one JSON object per second to file F
  --frame-trace F on quit, write the timeline of every rendered frame and every present to file F as JSON
                  lines
  --realtime-frames
                  run the render and present threads as time-constraint threads: their timed waits wake on time
                  instead of a few ms late, at some CPU, and they can starve other threads on a busy machine
  --gpu-profile   GPU time per pass by layer, fade side (in, out) and effects in the stats, and per layer and side on
                  the console (costs a little GPU time)
  --no-frame-guard  never draw layers at half resolution to keep the frame rate (the composition can also turn it off)
  --fsr-upscale   layers the frame guard draws at half resolution scale up with FSR 1 instead of bilinearly
                  (sharper, but costs more GPU time than half resolution saves on light chains)
  --debug-frame-bits  a strip of bars along the bottom of every output shows the render frame index in binary
                  (16 bits, most significant left, white = 1), so dropped or repeated frames are visible
  --opaque-outputs  outputs draw slices of the composition with no masks or soft edge without blending (the same
                  picture; for an A/B of the outputs' GPU time)
  --audio-input D audio input device by UID, name or part of a name; the default input when missing (default:
                  the default input; "system": all system audio; "app:NAME": one app's audio, by part of its
                  bundle ID or name; "off" captures nothing and never asks for the microphone; NOCTURNE_NO_AUDIO_INPUT=1 in
                  the environment keeps every input off, for tests and laptop runs)
  --audio-channels LIST
                  1-based input channels mixed to mono, e.g. 2 or 1,2 (default: all). With --state, the input
                  picked at runtime reopens on every start (a crash, a hang, a supervisor or login restart) while
                  --audio-input and --audio-channels stay as they were when it was picked; changed flags, or a
                  picked device no longer attached, open the flags' input
  --audio-gain G  auto, or a fixed gain in dB (default auto)
  --audio-latency MS
                  delay audio reactions by MS milliseconds (0..1000, default 0)
  --audio-log FILE
                  append the audio analysis as JSON lines at 10 Hz, with input events
  --state DIR     keep the composition and where the show is in DIR and resume them on start; record what is opened
                  or compiled for the supervisor's skip list, and skip what is on it
  --composition FILE
                  start on this composition file and play it; with --state, only when the saved state holds another
                  show: the saved state of the same FILE wins even after FILE changes (nocturne-ctl load reloads it),
                  and a copy of it goes to DIR/backups before FILE replaces it
  --media-root DIR
                  where the composition file's media paths start (default: the file's folder)
  --safe-mode REASON
                  show black (or --fallback-image) with REASON on every display and run nothing else
  --fallback-image FILE
                  the picture safe mode shows
  --node          start as a node: follow the main engine found on the network (Bonjour, _nocturne._tcp), copy its
                  composition and the media this machine lacks, and keep playing alone whenever it is away
  --follow MAIN   as --node, following the engine advertised as MAIN, or HOST[:PORT]; NOCTURNE_TOKEN in the
                  environment is the main machine's pairing token
  --name NAME     the name this engine advertises (default: the computer name)
  With --state, the cache sizes, the profile, protect-transitions and bake-static set at runtime (nocturne-ctl settings, the UI's
  Settings) are kept and come back on every start; these flags override what is kept.
  --record-path DIR
                  the folder recordings go to (default ~/Movies/Nocturne). With --state, the recording settings set at
                  runtime (nocturne-ctl record, the UI's Settings) are kept and come back on every start; these flags
                  override what is kept
  --record-reserve MB
                  a recording stops, and none starts, when less than the larger of MB and the percentage below stays
                  free on its disk (default 20480; nocturne-ctl record reserve changes it live)
  --record-reserve-percent N
                  the reserve as a percentage of the recording's disk (default 10)
  --mic-check     print the microphone permission (asking for it if it was never decided) and the input devices,
                  then quit; exit status 0 when access is granted
)";

namespace {

struct BadArgs {};

class Args {
public:
    explicit Args(const std::vector<std::string>& args) : args_(args) {}

    bool done() const { return at_ >= args_.size(); }
    const std::string& flag() { return args_[at_++]; }
    const char* value()
    {
        if (done())
            throw BadArgs{};
        return args_[at_++].c_str();
    }

private:
    const std::vector<std::string>& args_;
    size_t at_ = 0;
};

void readSlow(const std::string& slow, EngineConfig& c)
{
    const size_t colon = slow.rfind(':');
    if (colon == std::string::npos)
        throw BadArgs{};
    c.slowDisplay = slow.substr(0, colon);
    c.slowMs = std::atoi(slow.c_str() + colon + 1);
}

void appendChannels(const std::string& list, std::vector<int>& channels)
{
    for (size_t at = 0; at < list.size();) {
        const size_t comma = std::min(list.find(',', at), list.size());
        channels.push_back(std::atoi(list.substr(at, comma - at).c_str()));
        at = comma + 1;
    }
}

bool readDisplayFlag(const std::string& arg, Args& in, EngineConfig& c)
{
    if (arg == "--display") c.display = in.value();
    else if (arg == "--main") c.main = in.value();
    else if (arg == "--displays") c.listDisplays = true;
    else if (arg == "--windowed") c.windowed = true;
    else if (arg == "--virtual") c.virtualDisplays = std::atoi(in.value());
    else if (arg == "--offscreen") c.offscreen = true;
    else if (arg == "--slow") readSlow(in.value(), c);
    else if (arg == "--debug-frame-bits") c.debugFrameBits = true;
    else if (arg == "--opaque-outputs") c.opaqueOutputs = true;
    else return false;
    return true;
}

bool readRenderFlag(const std::string& arg, Args& in, EngineConfig& c)
{
    if (arg == "--seconds") c.seconds = std::atof(in.value());
    else if (arg == "--fps") c.fps = std::atof(in.value());
    else if (arg == "--size") {
        if (std::sscanf(in.value(), "%ux%u", &c.width, &c.height) != 2)
            throw BadArgs{};
    }
    else if (arg == "--clip-cache") c.clipCacheMB = size_t(std::atoll(in.value()));
    else if (arg == "--preload") c.preloadMB = size_t(std::atoll(in.value()));
    else if (arg == "--kept-previews") c.keptPreviews = size_t(std::atoll(in.value()));
    else if (arg == "--profile") {
        c.profile = in.value();
        if (c.profile != "smooth" && c.profile != "sharp")
            throw BadArgs{};
    }
    else if (arg == "--protect-transitions") {
        const std::string on = in.value();
        if (on != "on" && on != "off")
            throw BadArgs{};
        c.protectTransitions = on == "on";
    }
    else if (arg == "--bake-static") {
        const std::string on = in.value();
        if (on != "on" && on != "off")
            throw BadArgs{};
        c.bakeStatic = on == "on";
    }
    else if (arg == "--seed") c.seed = uint32_t(std::strtoul(in.value(), nullptr, 10));
    else if (arg == "--clip-previews") c.clipPreviews = true;
    else if (arg == "--stats-json") c.statsJson = in.value();
    else if (arg == "--frame-trace") c.frameTrace = in.value();
    else if (arg == "--realtime-frames") c.realtimeFrames = true;
    else if (arg == "--gpu-profile") c.gpuProfile = true;
    else if (arg == "--no-frame-guard") c.frameGuard = false;
    else if (arg == "--fsr-upscale") c.fsrUpscale = true;
    else return false;
    return true;
}

bool readAudioFlag(const std::string& arg, Args& in, EngineConfig& c)
{
    if (arg == "--audio-input") c.audioInput = in.value();
    else if (arg == "--audio-channels") appendChannels(in.value(), c.audioChannels);
    else if (arg == "--audio-gain") c.audioGain = in.value();
    else if (arg == "--audio-latency") c.audioLatencyMs = std::atof(in.value());
    else if (arg == "--audio-log") c.audioLog = in.value();
    else if (arg == "--mic-check") c.micCheck = true;
    else return false;
    return true;
}

bool readFileFlag(const std::string& arg, Args& in, EngineConfig& c)
{
    if (arg == "--library") c.library = in.value();
    else if (arg == "--presets") c.presets = in.value();
    else if (arg == "--macro-sets") c.macroSets = in.value();
    else if (arg == "--state") c.state = in.value();
    else if (arg == "--composition") c.composition = in.value();
    else if (arg == "--media-root") c.mediaRoot = in.value();
    else if (arg == "--safe-mode") c.safeMode = in.value();
    else if (arg == "--fallback-image") c.fallbackImage = in.value();
    else return false;
    return true;
}

bool readNetworkFlag(const std::string& arg, Args& in, EngineConfig& c)
{
    if (arg == "--port") c.port = uint16_t(std::atoi(in.value()));
    else if (arg == "--loopback") c.loopback = true;
    else if (arg == "--node") c.node = true;
    else if (arg == "--follow") {
        c.follow = in.value();
        c.node = true;
    }
    else if (arg == "--name") c.name = in.value();
    else return false;
    return true;
}

bool readRecordFlag(const std::string& arg, Args& in, EngineConfig& c)
{
    if (arg == "--record-path") c.recordPath = in.value();
    else if (arg == "--record-reserve") c.recordReserveMB = std::strtoull(in.value(), nullptr, 10);
    else if (arg == "--record-reserve-percent") c.recordReservePercent = std::atof(in.value());
    else return false;
    return true;
}

bool readFlag(const std::string& arg, Args& in, EngineConfig& c)
{
    return readDisplayFlag(arg, in, c) || readRenderFlag(arg, in, c) || readAudioFlag(arg, in, c) ||
           readFileFlag(arg, in, c) || readNetworkFlag(arg, in, c) || readRecordFlag(arg, in, c);
}

} // namespace

ParsedArgs parseEngineArgs(const std::vector<std::string>& args)
{
    ParsedArgs parsed;
    Args in(args);
    try {
        while (!in.done()) {
            const std::string& arg = in.flag();
            if (arg == "--help" || arg == "-h")
                return {ParsedArgs::Outcome::Help, parsed.config};
            if (!readFlag(arg, in, parsed.config))
                return {ParsedArgs::Outcome::Error, parsed.config};
        }
    } catch (const BadArgs&) {
        return {ParsedArgs::Outcome::Error, parsed.config};
    }
    return parsed;
}

std::filesystem::path engineConfigFile(const std::filesystem::path& stateDir)
{
    return stateDir / "engine.json";
}

EngineConfig loadEngineConfig(const std::filesystem::path& file)
{
    std::ifstream in(file);
    if (!in)
        throw std::runtime_error("cannot read " + file.string());
    const nlohmann::json j = nlohmann::json::parse(in);
    EngineConfig config = j.get<EngineConfig>();
    if (j.contains("recordReserveMB") && !j["recordReserveMB"].is_null())
        config.recordReserveMB = j["recordReserveMB"].get<uint64_t>();
    if (j.contains("recordReservePercent") && !j["recordReservePercent"].is_null())
        config.recordReservePercent = j["recordReservePercent"].get<double>();
    const auto size = [&](const char* key, std::optional<size_t>& value) {
        if (j.contains(key) && !j[key].is_null())
            value = j[key].get<size_t>();
    };
    size("clipCacheMB", config.clipCacheMB);
    size("preloadMB", config.preloadMB);
    size("keptPreviews", config.keptPreviews);
    if (j.contains("protectTransitions") && !j["protectTransitions"].is_null())
        config.protectTransitions = j["protectTransitions"].get<bool>();
    if (j.contains("bakeStatic") && !j["bakeStatic"].is_null())
        config.bakeStatic = j["bakeStatic"].get<bool>();
    return config;
}

void saveEngineConfig(const std::filesystem::path& file, const EngineConfig& config)
{
    std::ofstream out(file);
    nlohmann::json j = config;
    if (config.recordReserveMB)
        j["recordReserveMB"] = *config.recordReserveMB;
    if (config.recordReservePercent)
        j["recordReservePercent"] = *config.recordReservePercent;
    if (config.clipCacheMB)
        j["clipCacheMB"] = *config.clipCacheMB;
    if (config.preloadMB)
        j["preloadMB"] = *config.preloadMB;
    if (config.keptPreviews)
        j["keptPreviews"] = *config.keptPreviews;
    if (config.protectTransitions)
        j["protectTransitions"] = *config.protectTransitions;
    if (config.bakeStatic)
        j["bakeStatic"] = *config.bakeStatic;
    out << j.dump(2) << "\n";
    if (!out)
        throw std::runtime_error("cannot write " + file.string());
}

} // namespace azl::app
