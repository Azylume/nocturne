#include "app/engine_config.hpp"
#include "app/safe_mode.hpp"
#include "audio/audio_service.hpp"
#include "audio/return_audio.hpp"
#include "djlink/clock_follower.hpp"
#include "djlink/djlink_service.hpp"
#include "document/composition_file.hpp"
#include "effects/preset_service.hpp"
#include "engine/clock_service.hpp"
#include "engine/cue_service.hpp"
#include "engine/desk_service.hpp"
#include "engine/engine.hpp"
#include "engine/isf_library.hpp"
#include "engine/isf_service.hpp"
#include "engine/machine_settings.hpp"
#include "engine/machine_service.hpp"
#include "engine/modulation_service.hpp"
#include "engine/preview_service.hpp"
#include "engine/render_thread.hpp"
#include "engine/settings_service.hpp"
#include "engine/switch_log.hpp"
#include "engine/thread_priority.hpp"
#include "engine/trigger_service.hpp"
#include "engine/wall_cue_service.hpp"
#include "media/composition_media.hpp"
#include "media/composition_service.hpp"
#include "media/library_service.hpp"
#include "net/pairing.hpp"
#include "net/websocket.hpp"
#include "output/output_service.hpp"
#include "output/outputs.hpp"
#include "output/return_video_service.hpp"
#include "output/record_service.hpp"
#include "protocol/build_info.hpp"
#include "harness/fake_displays.hpp"
#include "platform/mac_displays.hpp"
#include "platform/profile.hpp"
#include "supervisor/crash_guard.hpp"
#include "supervisor/rotating_log.hpp"
#include "supervisor/state_saver.hpp"
#include "sync/discovery.hpp"
#include "sync/node_sync.hpp"
#include "sync/udp_clock.hpp"
#include "upload/media_sync.hpp"

#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <malloc/malloc.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>
#include <sys/resource.h>

namespace {

using azl::app::EngineConfig;

[[noreturn]] void usage(int code)
{
    std::fputs(azl::app::kEngineUsage, code ? stderr : stdout);
    std::exit(code);
}

EngineConfig parse(int argc, char** argv)
{
    const azl::app::ParsedArgs parsed = azl::app::parseEngineArgs({argv + 1, argv + argc});
    if (parsed.outcome == azl::app::ParsedArgs::Outcome::Help)
        usage(0);
    if (parsed.outcome == azl::app::ParsedArgs::Outcome::Error)
        usage(2);
    return parsed.config;
}

void quit()
{
    [NSApp stop:nil];
    NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                       location:NSZeroPoint
                                  modifierFlags:0
                                      timestamp:0
                                   windowNumber:0
                                        context:nil
                                        subtype:0
                                          data1:0
                                          data2:0];
    [NSApp postEvent:wake atStart:YES];
}

dispatch_source_t quitOnSignal(int sig)
{
    std::signal(sig, SIG_IGN);
    dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, uintptr_t(sig), 0,
                                                      dispatch_get_main_queue());
    dispatch_source_set_event_handler(source, ^{ quit(); });
    dispatch_resume(source);
    return source;
}

double gpuMsPerFrame(azl::OutputStats& o)
{
    const uint64_t frames = o.gpuFrames.exchange(0);
    const uint64_t micros = o.gpuMicros.exchange(0);
    return frames ? double(micros) / 1000.0 / double(frames) : 0.0;
}

nlohmann::json outputStats(azl::DisplayOutput& output, double s)
{
    auto& o = output.stats();
    return {{"display", output.display()},
            {"attached", output.attached()},
            {"main", output.main()},
            {"hz", o.refreshHz.load()},
            {"presents", o.presents.exchange(0) / s},
            {"shown", o.shown.exchange(0) / s},
            {"new", o.newFrames.exchange(0) / s},
            {"dropped", o.dropped.exchange(0)},
            {"maxIntervalMs", o.maxIntervalMicros.exchange(0) / 1000.0},
            {"hitches", o.hitches.exchange(0)},
            {"presentMaxMs", o.maxPresentMicros.exchange(0) / 1000.0},
            {"gpuMs", gpuMsPerFrame(o)}};
}

void printOutput(const nlohmann::json& o)
{
    if (!o["attached"]) {
        std::printf("  output %s: parked\n", o["display"].get<std::string>().c_str());
        return;
    }
    std::printf("  output %s (%s%.2f Hz): %.1f presents/s, %.1f shown/s, %.1f new frames/s, max interval %.1f ms, "
                "%llu hitches, %llu dropped\n",
                o["display"].get<std::string>().c_str(), o["main"] ? "main, " : "", o["hz"].get<double>(),
                o["presents"].get<double>(), o["shown"].get<double>(), o["new"].get<double>(),
                o["maxIntervalMs"].get<double>(), o["hitches"].get<unsigned long long>(),
                o["dropped"].get<unsigned long long>());
}

nlohmann::json takeMissesByLayer(azl::RenderStats& r)
{
    nlohmann::json misses = nlohmann::json::object();
    std::lock_guard lock(r.missesMutex);
    for (const auto& [layer, count] : r.missesByLayer)
        misses[std::to_string(layer)] = count;
    r.missesByLayer.clear();
    return misses;
}

// Milliseconds per frame that drew it, by pass label.
nlohmann::json gpuProfile(const std::map<std::string, azl::GpuProfile::Total>& totals)
{
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [label, total] : totals)
        out[label] = {{"ms", double(total.micros) / 1000.0 / double(total.frames)}, {"frames", total.frames}};
    return out;
}

// The layers' and sides' totals, the most expensive first.
void printGpuProfile(const nlohmann::json& profile)
{
    std::vector<std::pair<double, std::string>> sides;
    for (const auto& [label, value] : profile.items())
        if (label.find(" | ") == std::string::npos)
            sides.emplace_back(value["ms"].get<double>(), label);
    std::sort(sides.rbegin(), sides.rend());
    std::printf("  gpu ms by layer and side:");
    for (const auto& [ms, label] : sides)
        std::printf(" %s %.2f,", label.c_str(), ms);
    std::printf("\n");
}

double cpuSeconds()
{
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    auto seconds = [](const timeval& t) { return double(t.tv_sec) + double(t.tv_usec) / 1e6; };
    return seconds(usage.ru_utime) + seconds(usage.ru_stime);
}

void printAudio(const azl::AudioAnalysis& analysis, const azl::AudioInput& input, const azl::AudioFrame& last,
                const azl::AudioFrame& now, double s)
{
    const azl::AudioInputStatus status = input.status();
    const auto rate = [&](size_t k) { return double(now.onsets[k] - last.onsets[k]) / s; };
    std::printf("  audio %s (%s, %.0f Hz): level %.2f, kick %.1f/s, snare %.1f/s, hat %.1f/s, gain %.1f dB, "
                "%llu overruns%s%s\n",
                status.name.empty() ? "-" : status.name.c_str(), status.state.c_str(), status.sampleRate,
                now.values[size_t(azl::AudioFeature::Level)], rate(0), rate(1), rate(2), now.gainDb,
                analysis.overruns(), analysis.digitalSilence() ? ", digital silence (no capture permission?)" : "",
                now.live ? "" : ", no audio");
}

// What malloc holds for the process, beside RSS: a memory step that is not in malloc is in pages the frameworks map.
double mallocInUseMB()
{
    malloc_statistics_t stats{};
    malloc_zone_statistics(nullptr, &stats);
    return double(stats.size_in_use) / 1048576.0;
}

size_t threadCount()
{
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
        return 0;
    for (mach_msg_type_number_t i = 0; i < count; ++i)
        mach_port_deallocate(mach_task_self(), threads[i]);
    vm_deallocate(mach_task_self(), vm_address_t(threads), count * sizeof(thread_act_t));
    return count;
}

size_t portCount()
{
    mach_port_name_array_t names = nullptr;
    mach_port_type_array_t types = nullptr;
    mach_msg_type_number_t nameCount = 0, typeCount = 0;
    if (mach_port_names(mach_task_self(), &names, &nameCount, &types, &typeCount) != KERN_SUCCESS)
        return 0;
    vm_deallocate(mach_task_self(), vm_address_t(names), nameCount * sizeof(mach_port_name_t));
    vm_deallocate(mach_task_self(), vm_address_t(types), typeCount * sizeof(mach_port_type_t));
    return nameCount;
}

void printSync(const nlohmann::json& status)
{
    if (status["role"] == "main") {
        std::printf("  sync main engine \"%s\", %zu node(s)\n", status["name"].get<std::string>().c_str(),
                    status["nodes"].get<size_t>());
        return;
    }
    const nlohmann::json& n = status["node"];
    if (!n["connected"]) {
        std::printf("  sync node, main engine away: playing alone\n");
        return;
    }
    if (n["held"]) {
        std::printf("  sync node of %s, which has no show: keeping this one (node adopt takes the main's)\n",
                    n["main"].get<std::string>().c_str());
        return;
    }
    std::printf("  sync node of %s: %s, %zu media missing%s%s, copied %llu (%.1f MB), clock offset %s, round trip %.2f ms\n",
                n["main"].get<std::string>().c_str(), n["synced"] ? "synced" : "syncing", n["missing"].get<size_t>(),
                n["fetching"].get<std::string>().empty() ? "" : ", copying ",
                n["fetching"].get<std::string>().c_str(), n["files"].get<unsigned long long>(),
                n["bytes"].get<double>() / 1048576.0,
                n["offset"].is_null() ? "unknown" : (std::to_string(n["offset"].get<double>()) + " s").c_str(),
                n["roundTrip"].get<double>() * 1000.0);
}

void printRecord(const nlohmann::json& record)
{
    if (!record["recording"])
        return;
    std::printf("  record %s %s%s from %s: %.1f fps, %llu dropped\n", record["codec"].get<std::string>().c_str(),
                record["target"].get<std::string>().c_str(),
                record["hardware"].is_null() ? "" : record["hardware"].get<bool>() ? " (hardware)" : " (software)",
                record["pixels"].get<std::string>().c_str(), record["fps"].get<double>(),
                record["dropped"].get<unsigned long long>());
}

class StatsPrinter {
public:
    StatsPrinter(azl::Engine& engine, azl::Outputs& outputs, azl::RotatingLog* json, const azl::AudioAnalysis& analysis,
                 const azl::AudioInput& input, const azl::SyncRole& sync, azl::Recorder& recorder,
                 azl::PreviewService& previews, const azl::ReturnAudio& returnAudio)
        : engine_(engine), outputs_(outputs), json_(json), analysis_(analysis), input_(input), sync_(sync),
          recorder_(recorder), previews_(previews), clipPreviews_(previews.clipStats()), returnAudio_(returnAudio)
    {
    }

    void setReturnVideo(const azl::ReturnVideoService* video) { returnVideo_ = video; }
    void setDjlink(std::function<nlohmann::json()> djlink) { djlink_ = std::move(djlink); }

    // Main thread, like the outputs.
    void print()
    {
        const auto now = std::chrono::steady_clock::now();
        const double s = std::chrono::duration<double>(now - last_).count();
        last_ = now;
        const nlohmann::json stats = collect(s);
        const auto& r = stats["render"];
        const auto& d = stats["decode"];
        std::printf("render %.1f fps, gpu %.2f ms (previews %.2f ms, max %.2f, skipped %llu, video misses %llu, %llu "
                    "passes, %llu intermediates, %llu at half resolution) | compiled %llu pipelines, %llu cached (max "
                    "%.1f ms) | cpu %.0f%%\n",
                    r["fps"].get<double>(), r["gpuMs"].get<double>(), r["previews"]["gpuMs"].get<double>(),
                    r["gpuMaxMs"].get<double>(),
                    r["skipped"].get<unsigned long long>(), r["videoMisses"].get<unsigned long long>(),
                    r["passes"].get<unsigned long long>(), r["intermediates"].get<unsigned long long>(),
                    r["budgetLevel"].get<unsigned long long>(),
                    r["compiles"].get<unsigned long long>(), r["cachedPipelines"].get<unsigned long long>(),
                    r["compileMaxMs"].get<double>(),
                    stats["cpu"].get<double>());
        std::printf("  decode %.0f fps, busy %.0f%%, %llu seeks, %llu errors, %llu stalls, read max %.1f ms, cache %.0f "
                    "MB | clip previews %.1f/s, busy %.0f%%\n",
                    d["fps"].get<double>(), d["busy"].get<double>(), d["seeks"].get<unsigned long long>(),
                    d["errors"].get<unsigned long long>(), d["stalls"].get<unsigned long long>(),
                    d["readMaxMs"].get<double>(), d["cacheMB"].get<double>(),
                    stats["clipPreviews"]["fps"].get<double>(), stats["clipPreviews"]["busy"].get<double>());
        if (r.contains("gpuProfile"))
            printGpuProfile(r["gpuProfile"]);
        for (const auto& output : stats["outputs"])
            printOutput(output);
        const azl::AudioFrame audio = analysis_.latest();
        printAudio(analysis_, input_, audio_, audio, s);
        audio_ = audio;
        std::printf("  listen %llu listeners, %.1f kbit/s, %llu samples dropped\n",
                    stats["listen"]["listeners"].get<unsigned long long>(), stats["listen"]["kbps"].get<double>(),
                    stats["listen"]["dropped"].get<unsigned long long>());
        printSync(stats["sync"]);
        printRecord(stats["record"]);
        if (stats.contains("djlink"))
            std::printf("  %s\n", azl::djlink::statusLine(stats["djlink"]).c_str());
        std::fflush(stdout);
        if (json_)
            json_->write(stats.dump() + "\n");
        latest_ = stats;
    }

    // Main thread: the stats print() collected last; null before the first.
    const nlohmann::json& latest() const { return latest_; }

private:
    nlohmann::json collect(double s)
    {
        auto& r = engine_.stats();
        auto& c = engine_.compileStats();
        const azl::DecodeStats& d = engine_.decodeStats();
        const uint64_t decoded = d.frames.load();
        const uint64_t busy = d.busyMicros.load();
        const uint64_t seeks = d.seeks.load();
        const double cpu = cpuSeconds();
        const uint64_t rendered = r.rendered.exchange(0);
        nlohmann::json stats{
            {"t", std::chrono::duration<double>(last_ - start_).count()},
            {"unix", azl::unixTime()},
            {"cpu", (cpu - cpu_) / s * 100.0},
            {"render",
             {{"fps", rendered / s},
              {"gpuMs", rendered ? double(r.gpuMicros.exchange(0)) / 1000.0 / double(rendered) : 0.0},
              {"gpuMaxMs", r.maxGpuMicros.exchange(0) / 1000.0},
              {"maxGapMs", r.maxGapMicros.exchange(0) / 1000.0},
              {"cpuMaxMs", r.maxCpuMicros.exchange(0) / 1000.0},
              {"doneMaxMs", r.maxDoneMicros.exchange(0) / 1000.0},
              {"skipped", r.skipped.exchange(0)},
              {"videoMisses", r.videoMisses.exchange(0)},
              {"passes", r.passes.load()},
              {"intermediates", r.intermediates.load()},
              {"bakedPasses", r.bakedPasses.load()},
              {"budgetLevel", r.budgetLevel.load()},
              {"budgetChanges", r.budgetChanges.exchange(0)},
              {"compiles", c.pipelines.exchange(0)},
              {"cachedPipelines", c.archived.exchange(0)},
              {"compileMaxMs", c.maxMicros.exchange(0) / 1000.0},
              {"starting", r.starting.load()}}},
            {"decode",
             {{"fps", (decoded - decoded_) / s},
              {"busy", (busy - busy_) / s / 1e4},
              {"seeks", seeks - seeks_},
              {"errors", d.errors.load()},
              {"stalls", d.stalls.load()},
              {"readMaxMs", d.maxReadMicros.exchange(0) / 1000.0},
              {"cacheMB", d.cachedBytes.load() / 1048576.0},
              {"opens", d.opens.load()},
              {"closes", d.closes.load()},
              {"openMaxMs", d.maxOpenMicros.exchange(0) / 1000.0},
              {"closeMaxMs", d.maxCloseMicros.exchange(0) / 1000.0},
              {"liveReaders", azl::liveAssetReaders()}}},
            {"outputs", nlohmann::json::array()}};
        const uint64_t clipPictures = clipPreviews_.pictures.load();
        const uint64_t clipBusy = clipPreviews_.busyMicros.load();
        stats["clipPreviews"] = {{"fps", (clipPictures - clipPictures_) / s}, {"busy", (clipBusy - clipBusy_) / s / 1e4}};
        clipPictures_ = clipPictures;
        clipBusy_ = clipBusy;
        if (azl::ClipEffectsPreview* effects = previews_.clipEffects())
            stats["clipPreviews"].update(clipEffects(*effects, s));
        cpu_ = cpu;
        decoded_ = decoded;
        busy_ = busy;
        seeks_ = seeks;
        for (azl::DisplayOutput* output : outputs_.all())
            stats["outputs"].push_back(outputStats(*output, s));
        stats["render"]["previews"] = previews(rendered, s);
        stats["render"]["missesByLayer"] = takeMissesByLayer(r);
        if (auto profile = engine_.takeGpuProfile(); !profile.empty())
            stats["render"]["gpuProfile"] = gpuProfile(profile);
        stats["sizes"] = engine_.sizes();
        stats["sizes"].update(nlohmann::json(previews_.sizes()));
        stats["render"]["previews"]["targets"] = stats["sizes"]["previewTargets"];
        stats["sizes"]["threads"] = threadCount();
        stats["sizes"]["machPorts"] = portCount();
        stats["sizes"]["mallocMB"] = mallocInUseMB();
        stats["sync"] = sync_.status();
        stats["record"] = record(s);
        if (returnVideo_)
            stats["returnVideo"] = returnVideo(s);
        if (djlink_)
            stats["djlink"] = djlink_();
        const uint64_t listened = returnAudio_.bytes();
        stats["listen"] = {{"listeners", returnAudio_.listeners()},
                           {"kbps", double(listened - std::exchange(listenBytes_, listened)) * 8.0 / 1000.0 / s},
                           {"dropped", returnAudio_.dropped()}};
        return stats;
    }

    // Pictures and kbit sent per second to every watcher together, and GPU and encode time per picture.
    nlohmann::json returnVideo(double s)
    {
        const azl::ReturnVideoService::Stats& v = returnVideo_->stats();
        const uint64_t pictures = v.pictures.load() - std::exchange(videoPictures_, v.pictures.load());
        const uint64_t bytes = v.bytes.load() - std::exchange(videoBytes_, v.bytes.load());
        const uint64_t gpu = v.gpuMicros.load() - std::exchange(videoGpu_, v.gpuMicros.load());
        const uint64_t encode = v.encodeMicros.load() - std::exchange(videoEncode_, v.encodeMicros.load());
        return {{"watchers", returnVideo_->watchers()},
                {"fps", pictures / s},
                {"kbps", bytes * 8.0 / 1000.0 / s},
                {"gpuMs", pictures ? gpu / 1000.0 / double(pictures) : 0.0},
                {"encodeMs", pictures ? encode / 1000.0 / double(pictures) : 0.0}};
    }

    // GPU time on preview watches per rendered frame: passes the frames drew only for them (part of render.gpuMs), the
    // pictures painted on the preview queue and the clip previews' effects; and which kinds of source how many watches
    // show.
    nlohmann::json previews(uint64_t rendered, double s)
    {
        const auto perFrame = [rendered](uint64_t micros, uint64_t& last) {
            const uint64_t spent = micros - std::exchange(last, micros);
            return rendered ? spent / 1000.0 / double(rendered) : 0.0;
        };
        const azl::RenderStats& r = engine_.stats();
        const azl::PreviewService::PaintStats& paint = previews_.paintStats();
        const azl::ClipEffectsPreview* effects = previews_.clipEffects();
        const double frameMs = perFrame(r.previewGpuMicros.load(), previewGpu_);
        const double paintMs = perFrame(paint.gpuMicros.load(), paintGpu_);
        const double clipMs = perFrame(effects ? effects->stats().gpuMicros.load() : 0, clipEffectGpu_);
        const uint64_t painted = paint.pictures.load();
        return {{"gpuMs", frameMs + paintMs + clipMs},
                {"frameMs", frameMs},
                {"paintMs", paintMs},
                {"clipEffectsMs", clipMs},
                {"passes", r.previewPasses.load()},
                {"paints", (painted - std::exchange(paintPictures_, painted)) / s},
                {"watchers", previews_.watchers()}};
    }

    // Clip preview pictures drawn through their clip's effects, their GPU time, and the preview's own compiles.
    nlohmann::json clipEffects(azl::ClipEffectsPreview& effects, double s)
    {
        const uint64_t drawn = effects.stats().pictures.load();
        const uint64_t gpu = effects.stats().gpuMicros.load();
        azl::CompileStats& compiles = effects.compiler().stats();
        nlohmann::json json{{"effectFps", (drawn - effectPictures_) / s},
                            {"effectGpuMs", drawn > effectPictures_
                                                ? (gpu - effectGpu_) / 1000.0 / double(drawn - effectPictures_)
                                                : 0.0},
                            {"compiles", compiles.pipelines.exchange(0)},
                            {"compileMaxMs", compiles.maxMicros.exchange(0) / 1000.0}};
        effectPictures_ = drawn;
        effectGpu_ = gpu;
        return json;
    }

    // Frames recorded and dropped in this second; a new recording starts its counts over.
    nlohmann::json record(double s)
    {
        const azl::Recorder::Status status = recorder_.status();
        if (status.frames < recorded_ || status.dropped < dropped_)
            recorded_ = dropped_ = 0;
        nlohmann::json record{{"recording", status.recording},
                              {"fps", (status.frames - recorded_) / s},
                              {"dropped", status.dropped - dropped_},
                              {"codec", status.settings.codec},
                              {"target", status.settings.target},
                              {"hardware", status.hardware ? nlohmann::json(*status.hardware) : nlohmann::json()},
                              {"pixels", status.pixels}};
        recorded_ = status.frames;
        dropped_ = status.dropped;
        return record;
    }

    azl::Engine& engine_;
    azl::Outputs& outputs_;
    azl::RotatingLog* json_;
    const std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_ = start_;
    double cpu_ = cpuSeconds();
    const azl::AudioAnalysis& analysis_;
    const azl::AudioInput& input_;
    const azl::SyncRole& sync_;
    azl::Recorder& recorder_;
    uint64_t recorded_ = 0;
    uint64_t dropped_ = 0;
    azl::AudioFrame audio_;
    nlohmann::json latest_;
    uint64_t decoded_ = 0;
    uint64_t busy_ = 0;
    uint64_t seeks_ = 0;
    azl::PreviewService& previews_;
    const azl::PreviewService::ClipStats& clipPreviews_;
    const azl::ReturnVideoService* returnVideo_ = nullptr;
    std::function<nlohmann::json()> djlink_;
    uint64_t videoPictures_ = 0;
    uint64_t videoBytes_ = 0;
    uint64_t videoGpu_ = 0;
    uint64_t videoEncode_ = 0;
    const azl::ReturnAudio& returnAudio_;
    uint64_t listenBytes_ = 0;
    uint64_t clipPictures_ = 0;
    uint64_t effectPictures_ = 0;
    uint64_t effectGpu_ = 0;
    uint64_t clipBusy_ = 0;
    uint64_t previewGpu_ = 0;
    uint64_t paintGpu_ = 0;
    uint64_t clipEffectGpu_ = 0;
    uint64_t paintPictures_ = 0;
};

// Tells the supervisor that frames are being finished: a byte down its pipe whenever a new frame was published since
// the last look. When the supervisor is gone the engine quits, so it never lingers beside the next one.
class Heartbeat {
public:
    Heartbeat(const azl::Engine& engine, int fd) : thread_([this, &engine, fd] { run(engine, fd); }) {}
    ~Heartbeat()
    {
        running_ = false;
        thread_.join();
    }

private:
    void run(const azl::Engine& engine, int fd)
    {
        fcntl(fd, F_SETFL, O_NONBLOCK);
        uint64_t last = UINT64_MAX;
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const azl::FrameRef frame = engine.latestFrame();
            if (!frame || frame->index == last)
                continue;
            last = frame->index;
            if (::write(fd, "h", 1) < 0 && errno == EPIPE) {
                std::printf("supervisor gone, quitting\n");
                std::fflush(stdout);
                dispatch_async(dispatch_get_main_queue(), ^{ quit(); });
                return;
            }
        }
    }

    std::atomic<bool> running_{true};
    std::thread thread_;
};

// Serves the protocol, retrying in the background while the port is taken: the show plays without it meanwhile.
class Listener {
public:
    Listener(azl::DocumentServer& document, uint16_t port, std::string token, bool loopback = false)
        : document_(document), port_(port), token_(std::move(token)), loopback_(loopback),
          queue_(dispatch_queue_create("nocturne.listener", DISPATCH_QUEUE_SERIAL))
    {
        dispatch_async(queue_, ^{ attempt(); });
    }
    ~Listener()
    {
        stopped_ = true;
        dispatch_sync(queue_, ^{ server_.reset(); });
    }

private:
    void attempt()
    {
        if (stopped_)
            return;
        try {
            server_ = std::make_unique<azl::WebSocketServer>(document_, port_, azl::Pairing{token_}, loopback_);
            std::printf("protocol on ws://%s:%u\n", loopback_ ? "127.0.0.1" : "0.0.0.0", unsigned(server_->port()));
        } catch (const std::exception& e) {
            std::printf("protocol: %s, trying again\n", e.what());
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * int64_t(NSEC_PER_SEC)), queue_, ^{ attempt(); });
        }
        std::fflush(stdout);
    }

    azl::DocumentServer& document_;
    uint16_t port_;
    const std::string token_;
    const bool loopback_;
    dispatch_queue_t queue_;
    std::atomic<bool> stopped_{false};
    std::unique_ptr<azl::WebSocketServer> server_;
};

// The machine's pairing token, in the state folder; empty, so no client from another machine gets in, when it can be
// neither read nor made.
std::string pairingToken(const std::filesystem::path& state)
{
    const std::filesystem::path file = azl::tokenFile(state.empty() ? azl::defaultStateFolder() : state);
    try {
        std::string token = azl::machineToken(file);
        std::printf("protocol: clients on other machines present the pairing token in %s (nocturne-ctl token)\n",
                    file.c_str());
        return token;
    } catch (const std::exception& e) {
        std::printf("protocol: %s; only clients on this machine get in\n", e.what());
        return {};
    }
}

// The pairing token of the main engine a node follows.
std::string mainToken()
{
    const char* token = std::getenv("NOCTURNE_TOKEN");
    return token ? token : "";
}

// The main engine a node follows: the one advertised as --follow MAIN, else the first engine advertising itself as main,
// which gets no token unless it is on this machine; a --follow that names no advertised engine is HOST[:PORT].
std::optional<azl::Endpoint> locateMain(const azl::Browser& browser, const std::string& follow, const std::string& self,
                                        uint16_t port)
{
    for (const azl::Service& s : browser.services()) {
        auto role = s.txt.find("role");
        if (s.name != self && role != s.txt.end() && role->second == "main" && (follow.empty() || s.name == follow))
            return azl::Endpoint{s.name, s.host, s.port,
                                 azl::presentsToken(!follow.empty(), s.host) ? mainToken() : std::string()};
    }
    if (follow.empty())
        return std::nullopt;
    const size_t colon = follow.rfind(':');
    if (colon == std::string::npos)
        return azl::Endpoint{follow, follow, port, mainToken()};
    return azl::Endpoint{follow, follow.substr(0, colon), uint16_t(std::atoi(follow.c_str() + colon + 1)),
                         mainToken()};
}

const char* roleName(azl::Role role)
{
    return role == azl::Role::Main ? "main" : "node";
}

// One pipeline cache per GPU and build: a new build or another GPU starts an empty one, and the others are removed.
std::filesystem::path pipelineCache(const std::filesystem::path& stateDir, MTL::Device* device)
{
    std::string name = std::string(device->name()->utf8String()) + "-" + azl::buildId();
    std::replace_if(name.begin(), name.end(), [](unsigned char c) { return !std::isalnum(c) && c != '-'; }, '_');
    const std::filesystem::path dir = stateDir / "pipelines";
    const std::filesystem::path file = dir / (name + ".metallib");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        if (entry.path() != file)
            std::filesystem::remove(entry.path(), ec);
    return file;
}

// A state file that cannot be read is moved aside, never a reason not to start.
std::optional<azl::CompositionFile> loadState(const std::filesystem::path& file)
{
    try {
        std::vector<std::string> warnings;
        std::optional<azl::CompositionFile> state = azl::loadCompositionFile(file, warnings);
        for (const std::string& w : warnings)
            std::printf("state: warning: %s\n", w.c_str());
        return state;
    } catch (const std::exception& e) {
        std::printf("state: %s; moved aside, starting fresh\n", e.what());
        std::error_code ec;
        std::filesystem::rename(file, file.string() + ".bad", ec);
        return std::nullopt;
    }
}

std::optional<azl::Resume> resumeNow(const azl::Engine& engine, const azl::AudioInput& input,
                                     const azl::AudioChoice& commandLine)
{
    const azl::TransportState t = engine.transport();
    if (t.time <= 0.0)
        return std::nullopt;
    const azl::AudioInputStatus audio = input.status();
    return azl::Resume{azl::unixTime() - (azl::engineTime() - t.time), t.beat, t.tempo, t.pilots,
                       azl::ResumeAudio{{audio.requested, audio.channels}, commandLine}};
}

azl::SettingsFlags settingsFlags(const EngineConfig& opt)
{
    azl::SettingsFlags flags;
    if (!opt.profile.empty())
        flags.profile = azl::guardProfileFromName(opt.profile);
    flags.protectTransitions = opt.protectTransitions;
    flags.bakeStatic = opt.bakeStatic;
    flags.frameCacheMB = opt.clipCacheMB;
    flags.preloadMB = opt.preloadMB;
    flags.clipPreviews = opt.keptPreviews;
    return flags;
}

// The default composition with one screen showing it on `display`, or on the first attached display.
azl::Composition startingComposition(const azl::Displays& displays, std::string display)
{
    const auto attached = displays.attached();
    if (display.empty() && !attached.empty())
        display = attached.front().id;
    azl::Composition composition = azl::defaultComposition();
    composition.screens.push_back(
        {.id = 1, .name = "Screen 1", .display = display, .slices = {{.id = 1, .name = "Slice 1"}}});
    return composition;
}

// The --composition file's content hash and path, and the file loaded unless the saved state holds the same show: one
// started from the same path, or from the same content when it predates recording the path.
struct StartingFile {
    std::optional<std::string> source;
    std::optional<std::string> show;
    std::optional<azl::Composition> composition;
};

azl::ShowFolders showFolders(const EngineConfig& opt)
{
    const char* env = std::getenv("HOME");
    const std::filesystem::path home = env ? env : "/";
    azl::ShowFolders folders{home / "Nocturne/shows", {home / "Nocturne/shows", home}};
    if (!opt.mediaRoot.empty())
        folders.roots.push_back(std::filesystem::absolute(opt.mediaRoot));
    return folders;
}

StartingFile startingFile(const EngineConfig& opt, const std::optional<azl::CompositionFile>& saved,
                          azl::MediaLibrary& library, const azl::DisplayPlacement& displays)
{
    const std::optional<std::string> savedSource = saved ? saved->source : std::nullopt;
    const std::optional<std::string> savedShow = saved ? saved->show : std::nullopt;
    if (opt.composition.empty())
        return {savedSource, savedShow, std::nullopt};
    const std::filesystem::path file = std::filesystem::absolute(opt.composition).lexically_normal();
    try {
        const std::string source = azl::sha256File(file);
        if (saved && (savedShow ? *savedShow == file.string() : savedSource == source)) {
            std::printf("composition: %s is the show the saved state holds%s; continuing the saved state\n", file.c_str(),
                        savedSource == source ? "" : " (the file changed since; nocturne-ctl load reloads it)");
            return {savedSource, file.string(), std::nullopt};
        }
        azl::LoadedComposition loaded = azl::loadComposition(file, library, displays, opt.mediaRoot);
        std::printf("composition: loaded %s\n", file.c_str());
        for (const std::string& w : loaded.warnings)
            std::printf("composition: warning: %s\n", w.c_str());
        return {source, file.string(), std::move(loaded.composition)};
    } catch (const std::exception& e) {
        std::printf("composition: %s; not loaded\n", e.what());
        return {savedSource, savedShow, std::nullopt};
    }
}

// The show file the document belongs to: the --composition file just loaded, else the one the saved state recorded. A
// state that predates recording it belongs to the show it started from, with edits since as far as anyone knows.
std::optional<azl::ShowFile> startingShowFile(const StartingFile& start,
                                              const std::optional<azl::CompositionFile>& saved)
{
    if (start.composition)
        return azl::ShowFile{*start.show};
    if (!saved)
        return std::nullopt;
    if (saved->file)
        return saved->file;
    if (saved->show)
        return azl::ShowFile{*saved->show, true};
    return std::nullopt;
}

// The saved state, else the default composition, with each screen on this machine's display for its role; the default
// screen given --display keeps it. A --composition file comes placed by its load.
azl::Composition startingDocument(const StartingFile& start, const std::optional<azl::CompositionFile>& saved,
                                  const azl::Displays& displays, const std::string& display,
                                  const azl::RoleDisplays& roles)
{
    if (start.composition)
        return *start.composition;
    azl::Composition composition = saved ? saved->composition : startingComposition(displays, display);
    if (!saved && !display.empty())
        return composition;
    for (const std::string& line : azl::dropRemovedEffects(composition))
        std::printf("state: warning: removed effect dropped: %s\n", line.c_str());
    std::vector<std::string> warnings;
    azl::placeByRole(composition, roles, warnings);
    for (const std::string& w : warnings)
        std::printf("state: warning: %s\n", w.c_str());
    return composition;
}

// Any thread. The displays belong to the main thread; a main thread that does not answer within 2 s gives none.
std::vector<std::string> attachedOnMain(const azl::Displays& displays)
{
    if ([NSThread isMainThread])
        return azl::displayIds(displays);
    auto ids = std::make_shared<std::vector<std::string>>();
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    const azl::Displays* d = &displays;
    dispatch_async(dispatch_get_main_queue(), ^{
        *ids = azl::displayIds(*d);
        dispatch_semaphore_signal(done);
    });
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 2 * int64_t(NSEC_PER_SEC))))
        return {};
    return *ids;
}

void printDisplays(const azl::Displays& displays)
{
    for (const azl::DisplayInfo& d : displays.attached())
        std::printf("display %s (%s) %ux%u at %.0f Hz%s%s\n", d.id.c_str(), d.name.c_str(), d.width, d.height,
                    d.refreshHz, d.mirrorOf.empty() ? "" : ", mirrors ", d.mirrorOf.c_str());
}

azl::AudioSettings audioSettings(const EngineConfig& opt)
{
    azl::AudioSettings settings;
    settings.autoGain = opt.audioGain == "auto";
    if (!settings.autoGain)
        settings.gainDb = std::clamp(std::atof(opt.audioGain.c_str()), -48.0, 48.0);
    settings.latencyMs = std::clamp(opt.audioLatencyMs, 0.0, azl::AudioSettings::kMaxLatencyMs);
    return settings;
}

int micCheck()
{
    std::string permission = azl::AudioInput::permission();
    std::printf("microphone permission: %s\n", permission.c_str());
    std::fflush(stdout);
    if (permission == "notDetermined" && azl::AudioInput::disabled())
        std::printf("not asking: NOCTURNE_NO_AUDIO_INPUT is set\n");
    else if (permission == "notDetermined") {
        std::printf("asking for microphone access; answer the prompt on this Mac's screen\n");
        std::fflush(stdout);
        dispatch_semaphore_t answered = dispatch_semaphore_create(0);
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                 completionHandler:^(BOOL) { dispatch_semaphore_signal(answered); }];
        if (dispatch_semaphore_wait(answered, dispatch_time(DISPATCH_TIME_NOW, int64_t(300) * NSEC_PER_SEC)))
            std::printf("no answer within 5 minutes\n");
        permission = azl::AudioInput::permission();
        std::printf("microphone permission: %s\n", permission.c_str());
    }
    for (const azl::AudioDeviceInfo& d : azl::AudioInput::devices())
        std::printf("input %s%s: %d channels at %.0f Hz, uid %s\n", d.name.c_str(), d.isDefault ? " (default)" : "",
                    d.channels, d.sampleRate, d.uid.c_str());
    return permission == "authorized" ? 0 : 1;
}

std::unique_ptr<azl::Displays> makeDisplays(const EngineConfig& opt)
{
    if (!opt.offscreen)
        return std::make_unique<azl::MacDisplays>(opt.windowed, opt.virtualDisplays);
    auto displays = std::make_unique<azl::FakeDisplays>();
    for (int i = 1; i <= std::max(opt.virtualDisplays, 1); ++i)
        displays->attach({"window-" + std::to_string(i), "Offscreen " + std::to_string(i), 1920, 1080, 60.0});
    return displays;
}

int run(const EngineConfig& opt)
{
    azl::realtimeFrames() = opt.realtimeFrames;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:opt.offscreen ? NSApplicationActivationPolicyProhibited
                                             : NSApplicationActivationPolicyAccessory];

    // A show runs unattended: no App Nap, no display sleep.
    id activity = [NSProcessInfo.processInfo
        beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical | NSActivityIdleDisplaySleepDisabled
                          reason:@"live show"];
    const std::unique_ptr<azl::Displays> ownedDisplays = makeDisplays(opt);
    azl::Displays& displays = *ownedDisplays;
    printDisplays(displays);
    if (opt.listDisplays)
        return 0;

    const std::filesystem::path stateDir = opt.state;
    std::optional<azl::CompositionFile> saved;
    if (!opt.state.empty()) {
        std::filesystem::create_directories(stateDir);
        azl::crash::setup(stateDir / "inflight.json", stateDir / "skip.json", stateDir / "crash.json");
        azl::crash::installCrashHandlers();
        for (const azl::crash::Skip& skip : azl::crash::readSkips(stateDir / "skip.json"))
            std::printf("skip list: %s %s (%s)\n", skip.mark.kind.c_str(), skip.mark.name.c_str(), skip.reason.c_str());
        saved = loadState(stateDir / "composition.json");
    }

    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    azl::UserIsfLibrary userIsf(opt.state.empty() ? std::filesystem::path() : stateDir / "isf-user",
                                opt.state.empty() ? std::filesystem::path() : stateDir / "isf");
    azl::MediaLibrary library(opt.library);
    azl::KnownDisplays known(opt.state.empty() ? std::filesystem::path() : stateDir / "displays.json");
    azl::ScreenDisplays screenDisplays(opt.state.empty() ? std::filesystem::path() : stateDir / "screen-displays.json");
    StartingFile start =
        startingFile(opt, saved, library, {azl::displayIds(displays), known.ids(), false, screenDisplays.roles()});
    const std::optional<azl::ShowFile> showFile = startingShowFile(start, saved);
    if (start.composition)
        saved.reset();
    const std::string cache = opt.state.empty() ? "" : pipelineCache(stateDir, device.get()).string();
    const std::filesystem::path settingsFile = opt.state.empty() ? std::filesystem::path() : stateDir / "settings.json";
    const azl::StartingSettings starting =
        azl::startingSettings(azl::loadMachineSettings(settingsFile), settingsFlags(opt));
    std::unique_ptr<azl::FrameTrace> trace =
        opt.frameTrace.empty() ? nullptr : std::make_unique<azl::FrameTrace>(opt.frameTrace);
    azl::Engine engine(device.get(),
                       {.width = opt.width,
                        .height = opt.height,
                        .clipCacheBytes = size_t(starting.settings.frameCacheMB) << 20,
                        .preloadBytes = size_t(starting.settings.preloadMB) << 20,
                        .seed = opt.seed,
                        .debugFrameBits = opt.debugFrameBits,
                        .opaqueOutputs = opt.opaqueOutputs,
                        .holdStart = true,
                        .pipelineCache = cache.empty() ? nullptr : cache.c_str(),
                        .gpuProfile = opt.gpuProfile,
                        .fsrUpscale = opt.fsrUpscale,
                        .frameTrace = trace.get()},
                       &library);
    if (opt.frameGuard)
        engine.setFrameBudget(1.0 / opt.fps);
    azl::Outputs outputs(engine, displays, opt.main, &known);
    azl::Outputs* outputsOnMain = &outputs;
    azl::AudioAnalysis analysis(azl::engineTime);
    analysis.setSettings(audioSettings(opt));
    azl::AudioInput input(analysis);
    const azl::AudioChoice commandLineInput{opt.audioInput, opt.audioChannels};
    const std::filesystem::path audioDevicesFile =
        opt.state.empty() ? std::filesystem::path() : stateDir / "audio-devices.json";
    std::optional<azl::ResumeAudio> pickedInput = azl::AudioDeviceSettings(audioDevicesFile, true, 0.0).picked();
    if (!pickedInput && saved && saved->resume)
        pickedInput = saved->resume->audio;
    std::string audioLog;
    const azl::AudioChoice audio =
        azl::startingInput(commandLineInput, pickedInput, azl::AudioInput::devices(), audioLog);
    if (!audioLog.empty())
        std::printf("audio: %s\n", audioLog.c_str());
    if (azl::AudioInput::disabled())
        std::printf("audio: NOCTURNE_NO_AUDIO_INPUT is set; the input stays off\n");
    std::unique_ptr<azl::StateSaver> saver;
    if (!opt.state.empty())
        saver = std::make_unique<azl::StateSaver>(
            stateDir / "composition.json",
            [&engine, &input, commandLineInput] { return resumeNow(engine, input, commandLineInput); },
            saved ? saved->resume : std::nullopt, start.source, start.show);
    azl::StateSaver* saving = saver.get();
    if (saving && start.composition)
        saving->backup("load");
    std::atomic<azl::OutputService*> keeperOnMain{nullptr};
    azl::DocumentServer document(startingDocument(start, saved, displays, opt.display, screenDisplays.roles()),
                                 [&engine, outputsOnMain, &keeperOnMain, saving, &screenDisplays](auto composition) {
                                     screenDisplays.keep(*composition);
                                     engine.setComposition(composition);
                                     if (saving)
                                         saving->changed(composition);
                                     azl::OutputService* keeper = keeperOnMain.load();
                                     dispatch_async(dispatch_get_main_queue(), ^{
                                         outputsOnMain->setComposition(composition);
                                         if (keeper)
                                             keeper->compositionChanged(*composition);
                                     });
                                 });
    if (saving)
        document.watchFile([saving](const std::optional<azl::ShowFile>& file) { saving->fileChanged(file); });
    if (showFile)
        document.resumeFile(*showFile);
    if (saved && saved->resume) {
        const double beat = saved->resume->beatAt(azl::unixTime());
        engine.resume(beat, azl::engineTime(), saved->resume->pilots);
        std::printf("state: resumed at beat %.2f, saved %.1f s ago\n", beat, azl::unixTime() - saved->resume->savedAt);
    }
    engine.onChainFailure(
        [&document](const auto& chain, const auto& error) { document.broadcast(azl::msg::chainFailed(chain, error)); });
    azl::LibraryService libraryService(document, library, [&engine](const std::string& hash) { engine.mediaAdded(hash); });
    const azl::Displays* displaysOnMain = &displays;
    azl::CompositionService compositionService(
        document, library,
        [displaysOnMain, &known, &screenDisplays] {
            return azl::DisplayPlacement{attachedOnMain(*displaysOnMain), known.ids(), false, screenDisplays.roles()};
        },
        showFolders(opt), [saving] {
            if (saving)
                saving->backup("load");
        });
    azl::ClockService clockService(document, engine, azl::engineTime);
    azl::OutputService outputService(document, displays, outputs, known, screenDisplays,
                                     [](std::function<void()> work) {
                                         dispatch_async(dispatch_get_main_queue(), ^{ work(); });
                                     });
    keeperOnMain = &outputService;
    outputService.keep();
    // Escape in an output window frees that display: its screen goes to no display, as `screendisplay SCREEN none`
    // or a display coming back would (a machine-local placement, no undo step), the Map shows it, and the show runs
    // on. Cmd+Q there ends the engine. Main thread, like every display change.
    displays.onOutputKey([&document, &outputs](azl::Displays::OutputKey key, const std::string& display) {
        if (key == azl::Displays::OutputKey::quit) {
            quit();
            return;
        }
        azl::DisplayOutput* output = outputs.find(display);
        if (!output)
            return;
        try {
            document.perform(azl::SetScreenDisplay{output->screen(), std::string()}, "escape");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "escape: cannot free display %s: %s\n", display.c_str(), e.what());
        }
    });
    azl::TriggerService triggerService(document, engine, azl::engineTime);
    azl::CueService cueService(document, engine, azl::engineTime);
    std::unique_ptr<azl::WallCueService> wallCues;
    if (!opt.node)
        wallCues = std::make_unique<azl::WallCueService>(document, engine, azl::engineTime, azl::unixTime,
                                                         azl::systemUtcOffset, 0.25);
    azl::PresetService presetService(document, opt.presets);
    azl::IsfService isfService(document, userIsf, [&engine] { engine.refreshEffects(); });
    azl::AudioService audioService(document, analysis, &input, azl::engineTime, opt.audioLog, audioDevicesFile);
    audioService.setCommandLine(commandLineInput);
    azl::ReturnAudio returnAudio(document, analysis);
    input.select(audio.device, audio.channels);
    engine.setAudio(&analysis);
    azl::ModulationService modulationService(document, engine, opt.macroSets);
    azl::PreviewService previewService(document, engine);
    previewService.setClipLibrary(&library);
    previewService.setClipPreviews(opt.clipPreviews);
    azl::PreviewService* previewing = &previewService;
    azl::ReturnVideoService returnVideo(document, engine);
    azl::applySettings(starting.settings, engine, previewService);
    azl::SettingsService settingsService(
        document, starting, settingsFile,
        [&engine, previewing](const azl::MachineSettings& s) { azl::applySettings(s, engine, *previewing); },
        [&engine, previewing] { return azl::settingsUse(engine, *previewing); });
    if (azl::flagsOverridePick(commandLineInput, pickedInput))
        settingsService.overridden("audioInput", "--audio-input " + (opt.audioInput.empty() ? std::string("(default)")
                                                                                             : opt.audioInput));
    const std::filesystem::path recordFile = opt.state.empty() ? std::filesystem::path() : stateDir / "record.json";
    azl::Recorder::Settings recording = azl::RecordService::load(recordFile, {});
    if (!opt.recordPath.empty())
        recording.path = std::filesystem::absolute(opt.recordPath);
    if (opt.recordReserveMB)
        recording.reserveBytes = *opt.recordReserveMB << 20;
    if (opt.recordReservePercent)
        recording.reservePercent = *opt.recordReservePercent;
    if (const std::string problem = azl::Recorder::folderProblem(recording.path); !problem.empty())
        std::printf("record: %s; recordings refuse to start until it is back or another folder is set\n",
                    problem.c_str());
    azl::Recorder::recover(azl::Recorder::movieFile(recording.path).parent_path());
    azl::RecordService recordService(document, engine, opt.fps, recording, recordFile);
    recordService.recorder().setAudio(&analysis, [&input] { return azl::audioSourceOf(input.status()); });
    std::unique_ptr<azl::SwitchLog> switchLog;
    if (!opt.node)
        switchLog = std::make_unique<azl::SwitchLog>(
            document, azl::SwitchLog::Options{.now = azl::engineTime,
                                              .wall = azl::unixTime,
                                              .folder = [&recordService] {
                                                  return azl::Recorder::movieFile(recordService.settings().path)
                                                      .parent_path();
                                              },
                                              .recorder = &recordService.recorder()});
    std::unique_ptr<azl::DeskService> desk;
    if (!opt.node)
        desk = std::make_unique<azl::DeskService>(
            document, azl::DeskService::Options{.wall = azl::unixTime,
                                                .file = opt.state.empty() ? std::filesystem::path()
                                                                          : stateDir / "desk.json"});
    azl::djlink::Service djlink(document, azl::engineTime,
                                opt.state.empty() ? std::filesystem::path() : stateDir / "djlink.json");
    std::unique_ptr<azl::MediaSync> mediaSync;
    if (!opt.node) {
        azl::MediaSyncOptions options{
            .settings = opt.state.empty() ? std::filesystem::path() : stateDir / "media-sync.json",
            .folder = library.root() / "Downloads"};
        try {
            options.client = std::make_shared<azl::S3Client>(azl::loadVenueR2Config());
        } catch (const std::exception& e) {
            options.clientError = e.what();
        }
        mediaSync = std::make_unique<azl::MediaSync>(
            document, library, [&engine] { return engine.composition(); }, azl::unixTime, std::move(options),
            [&engine](const std::string& hash) { engine.mediaAdded(hash); });
    }
    document.allowRemoteFiles([folders = showFolders(opt), &opt, &library, &recordService, stateDir] {
        std::vector<std::filesystem::path> roots{folders.shows, folders.shows.parent_path() / "media", library.root(),
                                                 recordService.settings().path,
                                                 stateDir.empty() ? azl::defaultStateFolder() : stateDir};
        if (!opt.mediaRoot.empty())
            roots.push_back(std::filesystem::absolute(opt.mediaRoot));
        return roots;
    });
    Listener listener(document, opt.port, pairingToken(stateDir), opt.loopback);
    std::unique_ptr<azl::ClockResponder> clockResponder;
    try {
        clockResponder = std::make_unique<azl::ClockResponder>(opt.port, azl::engineTime);
    } catch (const std::exception& e) {
        std::printf("sync: no clock responder: %s\n", e.what());
    }
    const std::string name = opt.name.empty() ? azl::machineName() : opt.name;
    const azl::Role role = opt.node ? azl::Role::Node : azl::Role::Main;
    azl::Browser browser;
    azl::Advertiser advertiser(name, opt.port, {{"role", roleName(role)}});
    const azl::Browser* browsing = &browser;
    azl::Advertiser* advertising = &advertiser;
    azl::SyncRole sync(
        document, engine, library, azl::engineTime,
        {.role = role,
         .name = name,
         .clockPort = clockResponder ? clockResponder->port() : uint16_t(0),
         .fresh = !start.composition && !saved,
         .locate = [browsing, follow = opt.follow, name, port = opt.port] {
             return locateMain(*browsing, follow, name, port);
         },
         .clock = [](const azl::Endpoint& main, uint16_t port) -> std::unique_ptr<azl::ClockLink> {
             return std::make_unique<azl::ClockPinger>(main.host, port, azl::engineTime);
         },
         .changed = [advertising](azl::Role now) { advertising->update({{"role", roleName(now)}}); }});
    azl::djlink::ClockFollower djClock(djlink, engine, document, azl::engineTime,
                                       {.allowed = [&sync] { return sync.role() == azl::Role::Main; }});
    auto render = std::make_unique<azl::RenderThread>(engine, opt.fps, &outputs.vsync());
    std::unique_ptr<Heartbeat> heartbeat;
    if (const char* fd = std::getenv("NOCTURNE_HEARTBEAT_FD"))
        heartbeat = std::make_unique<Heartbeat>(engine, std::atoi(fd));
    std::printf("device %s | composition %ux%u at %.0f fps%s\n", device->name()->utf8String(), opt.width, opt.height,
                opt.fps, opt.windowed ? " | windowed" : "");
    std::printf("media library %s | clip cache %llu MB | seed %u%s%s\n", opt.library.c_str(),
                (unsigned long long)starting.settings.frameCacheMB, opt.seed,
                opt.state.empty() ? "" : " | state ", opt.state.c_str());
    std::fflush(stdout);

    if (!opt.slowDisplay.empty())
        outputs.setDelay(opt.slowDisplay, opt.slowMs);

    std::unique_ptr<azl::RotatingLog> json = opt.statsJson.empty() ? nullptr
                                                                    : std::make_unique<azl::RotatingLog>(opt.statsJson);
    StatsPrinter stats(engine, outputs, json.get(), analysis, input, sync, recordService.recorder(),
                       previewService, returnAudio);
    stats.setReturnVideo(&returnVideo);
    const auto djlinkState = [&djlink, &djClock] {
        azl::Message state = djlink.summary();
        state["following"] = djClock.following();
        return state;
    };
    stats.setDjlink(djlinkState);
    StatsPrinter* printer = &stats;
    int exitCode = 0;
    azl::MachineActions::Restart restart = azl::restartRequest(&exitCode, quit);
    const azl::UpdateSource update = restart && !opt.state.empty() ? azl::installedUpdate(stateDir) : azl::UpdateSource{};
    azl::MachineService machine(
        document, engine, displays, &known, azl::engineTime,
        [](std::function<void()> work) { dispatch_async(dispatch_get_main_queue(), ^{ work(); }); },
        {.stats = [printer] { return printer->latest(); },
         .thermal = azl::readThermal,
         .djlink = djlinkState,
         .media = [&mediaSync] { return mediaSync ? mediaSync->status() : azl::Message(nullptr); },
         .input = &input,
         .identify = [outputsOnMain](double seconds) { outputsOnMain->identify({}, seconds); },
         .unmirror = [&outputService] { return outputService.unmirrorNow(); },
         .showLock = opt.state.empty() ? std::filesystem::path() : stateDir / "show-lock.json",
         .restart = std::move(restart),
         .update = update,
         .wall = azl::unixTime});
    machine.setAudio(&analysis);
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 100);
    dispatch_source_set_event_handler(timer, ^{ printer->print(); });
    dispatch_resume(timer);

    dispatch_source_t onInt = quitOnSignal(SIGINT);
    dispatch_source_t onTerm = quitOnSignal(SIGTERM);
    if (opt.seconds > 0)
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(opt.seconds * NSEC_PER_SEC)), dispatch_get_main_queue(),
                       ^{ quit(); });

    [NSApp run];

    dispatch_source_cancel(timer);
    dispatch_source_cancel(onInt);
    dispatch_source_cancel(onTerm);
    heartbeat.reset();
    render.reset();
    engine.onChainFailure(nullptr);
    engine.waitForCompiles();
    if (trace && !trace->write())
        std::fprintf(stderr, "nocturne: could not write the frame trace to %s\n", opt.frameTrace.c_str());
    [NSProcessInfo.processInfo endActivity:activity];
    std::printf("stopped\n");
    return exitCode;
}

} // namespace

int main(int argc, char** argv)
{
    AZL_THREAD_NAME("main");
    azl::becomeResponsibleProcess(argv);
    std::signal(SIGPIPE, SIG_IGN);
    @autoreleasepool {
        const EngineConfig opt = parse(argc, argv);
        if (opt.micCheck)
            return micCheck();
        if (!opt.safeMode.empty())
            return azl::runSafeMode(opt);
        try {
            return run(opt);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "nocturne: %s\n", e.what());
            return 1;
        }
    }
}
